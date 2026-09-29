#include <LikesProgram/Http/Http3QuicAdapter.hpp>
#include <LikesProgram/Http/HttpBodyBudget.hpp>
#include <LikesProgram/Http/HttpBodyProducer.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>

namespace {
    using LikesProgram::Http::Http3QuicAction;
    using LikesProgram::Http::Http3QuicActionKind;
    using LikesProgram::Http::Http3QuicTransportFeedback;
    using LikesProgram::Http::Http3QuicTransportFeedbackKind;

    constexpr std::uint64_t kMaximumQuicVarInt =
        (std::uint64_t{ 1 } << 62) - 1;

    struct ActionQueueLimits {
        std::size_t maxActions = 0;
        std::size_t maxPayloadBytes = 0;
    };

    struct DeferredActionRejection {
        bool available = false;
        Http3QuicActionKind kind = Http3QuicActionKind::StreamData;
        std::uint64_t actionId = 0;
        std::uint64_t streamId = 0;
    };

    class BoundedActionQueue final
        : public LikesProgram::Http::Http3QuicActionSink {
    public:
        explicit BoundedActionQueue(ActionQueueLimits limits) noexcept
            : m_limits(limits) { }

        void Submit(const Http3QuicAction& action) noexcept override {
            ++m_submissions;
            if (!IsValid(action)
                || m_deferred.available
                || !HasCapacityFor(action.payload.size())) {
                Defer(action);
                return;
            }

            try {
                m_actions.push_back(action);
                m_payloadBytes += action.payload.size();
            }
            catch (...) {
                Defer(action);
            }
        }

        bool Empty() const noexcept { return m_actions.empty(); }
        std::size_t Size() const noexcept { return m_actions.size(); }
        std::size_t PayloadBytes() const noexcept { return m_payloadBytes; }
        std::size_t Submissions() const noexcept { return m_submissions; }
        std::size_t Refusals() const noexcept { return m_refusals; }
        std::size_t LostRefusals() const noexcept { return m_lostRefusals; }

        bool HasCapacityFor(std::size_t payloadBytes) const noexcept {
            return !m_deferred.available
                && m_limits.maxActions != 0
                && m_actions.size() < m_limits.maxActions
                && payloadBytes <= m_limits.maxPayloadBytes
                && m_payloadBytes
                    <= m_limits.maxPayloadBytes - payloadBytes;
        }

        const Http3QuicAction* Front() const noexcept {
            return m_actions.empty() ? nullptr : &m_actions.front();
        }

        void PopFront() noexcept {
            if (m_actions.empty()) return;
            m_payloadBytes -= m_actions.front().payload.size();
            m_actions.pop_front();
        }

        DeferredActionRejection TakeDeferredRejection() noexcept {
            const auto result = m_deferred;
            m_deferred = {};
            return result;
        }

    private:
        static bool IsValid(const Http3QuicAction& action) noexcept {
            if (action.actionId == 0
                || action.streamId > kMaximumQuicVarInt
                || action.errorCode > kMaximumQuicVarInt
                || action.flowCredit > kMaximumQuicVarInt) {
                return false;
            }

            switch (action.kind) {
            case Http3QuicActionKind::StreamData:
                return !action.payload.empty()
                    && action.errorCode == 0 && action.flowCredit == 0;
            case Http3QuicActionKind::StreamFin:
                return action.payload.empty()
                    && action.errorCode == 0 && action.flowCredit == 0;
            case Http3QuicActionKind::ResetStream:
            case Http3QuicActionKind::StopSending:
                return action.payload.empty() && action.flowCredit == 0;
            case Http3QuicActionKind::CloseConnection:
                return action.streamId == 0 && action.payload.empty()
                    && action.flowCredit == 0;
            case Http3QuicActionKind::StreamReceiveCredit:
                return action.payload.empty() && action.errorCode == 0
                    && action.flowCredit != 0;
            case Http3QuicActionKind::ConnectionReceiveCredit:
                return action.streamId == 0 && action.payload.empty()
                    && action.errorCode == 0 && action.flowCredit != 0;
            }
            return false;
        }

        void Defer(const Http3QuicAction& action) noexcept {
            ++m_refusals;
            if (m_deferred.available) {
                ++m_lostRefusals;
                return;
            }
            m_deferred = { true, action.kind, action.actionId, action.streamId };
        }

        ActionQueueLimits m_limits;
        std::deque<Http3QuicAction> m_actions;
        std::size_t m_payloadBytes = 0;
        DeferredActionRejection m_deferred;
        std::size_t m_submissions = 0;
        std::size_t m_refusals = 0;
        std::size_t m_lostRefusals = 0;
    };

    class RejectedDataReplayScheduler {
    public:
        RejectedDataReplayScheduler(
            LikesProgram::Http::Http3QuicAdapter& adapter,
            LikesProgram::Http::HttpBodyProducer& producer,
            BoundedActionQueue& queue) noexcept
            : m_adapter(adapter), m_producer(producer), m_queue(queue) { }

        bool SendNext(std::uint64_t streamId, std::size_t maxBytes) {
            const auto prepared = m_producer.PreparePull(maxBytes);
            if (!prepared.IsOk()) return false;
            if (!prepared.Value().available) return true;
            if (!m_adapter.SendPreparedStreamData(
                    streamId, m_producer, prepared.Value().id).IsOk()) {
                return false;
            }
            return ConsumeDeferredRejection() && TryScheduleReplay();
        }

        bool AcceptFront() {
            const auto* action = m_queue.Front();
            if (action == nullptr) return false;
            const auto actionId = action->actionId;
            const auto acceptedBytes = action->kind
                    == Http3QuicActionKind::StreamData
                ? action->payload.size() : 0;
            if (!m_adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    actionId, acceptedBytes, 0 }).IsOk()) {
                return false;
            }
            m_queue.PopFront();
            return TryScheduleReplay();
        }

        bool RejectFront(std::uint64_t errorCode) {
            const auto* action = m_queue.Front();
            if (action == nullptr) return false;
            const auto kind = action->kind;
            const auto actionId = action->actionId;
            const Http3QuicTransportFeedback feedback{
                Http3QuicTransportFeedbackKind::ActionRejected,
                actionId, 0, errorCode };
            const auto result = kind == Http3QuicActionKind::StreamData
                ? m_adapter.FeedTransportFeedbackAndRequeueRejectedData(
                    feedback, m_producer)
                : m_adapter.FeedTransportFeedback(feedback);
            if (!result.IsOk()) return false;
            m_queue.PopFront();
            return TryScheduleReplay();
        }

        std::uint64_t LastDeferredRejectedActionId() const noexcept {
            return m_lastDeferredRejectedActionId;
        }

    private:
        bool ConsumeDeferredRejection() {
            const auto rejected = m_queue.TakeDeferredRejection();
            if (!rejected.available) return true;
            m_lastDeferredRejectedActionId = rejected.actionId;
            const Http3QuicTransportFeedback feedback{
                Http3QuicTransportFeedbackKind::ActionRejected,
                rejected.actionId, 0, 0 };
            if (rejected.kind == Http3QuicActionKind::StreamData) {
                return m_adapter.FeedTransportFeedbackAndRequeueRejectedData(
                    feedback, m_producer).IsOk();
            }
            return m_adapter.FeedTransportFeedback(feedback).IsOk();
        }

        bool TryScheduleReplay() {
            const auto replay = m_producer.RejectedDataReplay();
            if (!replay.available || !m_queue.HasCapacityFor(replay.bytes)) {
                return true;
            }
            const auto prepared = m_producer.PrepareRejectedDataReplay();
            if (!prepared.IsOk()
                || !prepared.Value().available
                || prepared.Value().payload.size() != replay.bytes) {
                return false;
            }
            if (!m_adapter.SendPreparedStreamData(
                    replay.streamId, m_producer, prepared.Value().id).IsOk()) {
                return false;
            }
            return ConsumeDeferredRejection();
        }

        LikesProgram::Http::Http3QuicAdapter& m_adapter;
        LikesProgram::Http::HttpBodyProducer& m_producer;
        BoundedActionQueue& m_queue;
        std::uint64_t m_lastDeferredRejectedActionId = 0;
    };

    bool PushAll(
        LikesProgram::Http::HttpBodyProducer& producer,
        const std::vector<std::uint8_t>& bytes) {
        const auto pushed = producer.Push(bytes.data(), bytes.size());
        return pushed.IsOk() && pushed.Value() == bytes.size();
    }

    bool Ready(
        LikesProgram::Http::Http3QuicAdapter& adapter,
        std::uint64_t streamId,
        std::uint64_t credit) {
        using LikesProgram::Http::Http3QuicEventKind;
        return adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
            && adapter.Feed({ Http3QuicEventKind::ConnectionSendCredit,
                0, 0, {}, credit }).IsOk()
            && adapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                streamId, 0, {}, credit }).IsOk();
    }

    bool FrontMatches(
        const BoundedActionQueue& queue,
        Http3QuicActionKind kind,
        std::uint64_t streamId,
        const std::vector<std::uint8_t>& payload) {
        const auto* action = queue.Front();
        return action != nullptr && action->kind == kind
            && action->actionId != 0 && action->streamId == streamId
            && action->payload == payload;
    }
}

int main() {
    using LikesProgram::Http::Http3QuicActionKind;

    BoundedActionQueue validationQueue({ 1, 4 });
    Http3QuicAction invalid;
    invalid.actionId = 0;
    invalid.payload = { 'x' };
    validationQueue.Submit(invalid);
    const auto invalidRejection = validationQueue.TakeDeferredRejection();
    if (!invalidRejection.available
        || invalidRejection.actionId != 0
        || !validationQueue.Empty()
        || validationQueue.Refusals() != 1
        || validationQueue.LostRefusals() != 0) {
        return 1;
    }
    invalid.actionId = 1;
    invalid.payload = { '1', '2', '3', '4', '5' };
    validationQueue.Submit(invalid);
    const auto oversizedRejection = validationQueue.TakeDeferredRejection();
    if (!oversizedRejection.available
        || oversizedRejection.actionId != 1
        || !validationQueue.Empty()
        || validationQueue.Refusals() != 2
        || validationQueue.LostRefusals() != 0) {
        return 1;
    }

    const std::uint64_t streamId = 4;
    const std::vector<std::uint8_t> rejectedBytes{ 'p', 'a', 'y' };
    const std::vector<std::uint8_t> tailBytes{ 'x', 'y' };
    LikesProgram::Http::HttpBodyBudget budget({ 16, 16 });
    LikesProgram::Http::HttpBodyProducer producer({ 16, 1, 16 });
    BoundedActionQueue queue({ 2, 16 });
    LikesProgram::Http::Http3QuicAdapter adapter(&queue);
    RejectedDataReplayScheduler scheduler(adapter, producer, queue);
    if (!producer.AttachBudget(&budget, streamId).IsOk()
        || !adapter.AttachBodyBudget(&budget).IsOk()
        || !Ready(adapter, streamId, 16)
        || !PushAll(producer, rejectedBytes)
        || !scheduler.SendNext(streamId, rejectedBytes.size())
        || !PushAll(producer, tailBytes)
        || !FrontMatches(queue, Http3QuicActionKind::StreamData,
            streamId, rejectedBytes)) {
        return 2;
    }
    const auto firstActionId = queue.Front()->actionId;
    if (adapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            firstActionId + 100, rejectedBytes.size(), 0 }).IsOk()
        || queue.Size() != 1
        || adapter.Snapshot().pendingDataActions != 1
        || budget.ReservedBytes(streamId)
            != rejectedBytes.size() + tailBytes.size()) {
        return 3;
    }
    if (!scheduler.RejectFront(0x71)
        || !FrontMatches(queue, Http3QuicActionKind::StreamData,
            streamId, rejectedBytes)
        || queue.Front()->actionId <= firstActionId
        || producer.BufferedBytes() != tailBytes.size()
        || producer.HasRejectedDataReplay()
        || adapter.SendCredit(streamId).Value().availableCredit != 13
        || budget.ReservedBytes(streamId)
            != rejectedBytes.size() + tailBytes.size()) {
        return 4;
    }
    if (!scheduler.AcceptFront()
        || !queue.Empty()
        || budget.ReservedBytes(streamId) != tailBytes.size()
        || !scheduler.SendNext(streamId, tailBytes.size())
        || !FrontMatches(queue, Http3QuicActionKind::StreamData,
            streamId, tailBytes)
        || !scheduler.AcceptFront()
        || budget.ReservedBytes() != 0
        || producer.BufferedBytes() != 0
        || queue.PayloadBytes() != 0) {
        return 5;
    }

    const std::uint64_t boundedStreamId = 8;
    const std::vector<std::uint8_t> firstBounded{ 'a' };
    const std::vector<std::uint8_t> secondBounded{ 'b', 'c' };
    LikesProgram::Http::HttpBodyBudget boundedBudget({ 8, 8 });
    LikesProgram::Http::HttpBodyProducer boundedProducer({ 8, 1, 8 });
    BoundedActionQueue boundedQueue({ 1, 8 });
    LikesProgram::Http::Http3QuicAdapter boundedAdapter(&boundedQueue);
    RejectedDataReplayScheduler boundedScheduler(
        boundedAdapter, boundedProducer, boundedQueue);
    if (!boundedProducer.AttachBudget(&boundedBudget, boundedStreamId).IsOk()
        || !boundedAdapter.AttachBodyBudget(&boundedBudget).IsOk()
        || !Ready(boundedAdapter, boundedStreamId, 8)
        || !PushAll(boundedProducer, firstBounded)
        || !boundedScheduler.SendNext(boundedStreamId, firstBounded.size())
        || !PushAll(boundedProducer, secondBounded)
        || !boundedScheduler.SendNext(boundedStreamId, secondBounded.size())
        || boundedQueue.Size() != 1
        || boundedQueue.Refusals() != 1
        || boundedQueue.LostRefusals() != 0
        || boundedScheduler.LastDeferredRejectedActionId() == 0
        || !boundedProducer.HasRejectedDataReplay()
        || boundedProducer.RejectedDataReplayBytes() != secondBounded.size()
        || boundedBudget.ReservedBytes(boundedStreamId) != 3) {
        return 6;
    }
    const auto refusedActionId =
        boundedScheduler.LastDeferredRejectedActionId();
    if (!boundedScheduler.AcceptFront()
        || !FrontMatches(boundedQueue, Http3QuicActionKind::StreamData,
            boundedStreamId, secondBounded)
        || boundedQueue.Front()->actionId <= refusedActionId
        || boundedProducer.HasRejectedDataReplay()
        || boundedBudget.ReservedBytes(boundedStreamId)
            != secondBounded.size()
        || !boundedScheduler.AcceptFront()
        || !boundedQueue.Empty()
        || boundedBudget.ReservedBytes() != 0) {
        return 7;
    }

    LikesProgram::Http::HttpBodyProducer controlProducer({ 8, 1, 8 });
    BoundedActionQueue controlQueue({ 1, 8 });
    LikesProgram::Http::Http3QuicAdapter controlAdapter(&controlQueue);
    RejectedDataReplayScheduler controlScheduler(
        controlAdapter, controlProducer, controlQueue);
    using LikesProgram::Http::Http3QuicEventKind;
    if (!controlAdapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !controlAdapter.Feed({ Http3QuicEventKind::StreamData,
            0, 0, { 'x' } }).IsOk()) {
        return 80;
    }
    if (!controlAdapter.UpdateConnectionReceiveCredit(64).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind
            != Http3QuicActionKind::ConnectionReceiveCredit
        || !controlScheduler.AcceptFront()) {
        return 81;
    }
    if (!controlAdapter.UpdateStreamReceiveCredit(0, 32).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind
            != Http3QuicActionKind::StreamReceiveCredit
        || !controlScheduler.AcceptFront()) {
        return 82;
    }
    if (!controlAdapter.SendStreamFin(4).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind != Http3QuicActionKind::StreamFin
        || !controlScheduler.AcceptFront()) {
        return 83;
    }
    if (!controlAdapter.ResetStream(8, 0x10).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind != Http3QuicActionKind::ResetStream
        || !controlScheduler.AcceptFront()) {
        return 84;
    }
    if (!controlAdapter.StopSending(12, 0x11).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind != Http3QuicActionKind::StopSending
        || !controlScheduler.AcceptFront()) {
        return 85;
    }
    if (!controlAdapter.Close(0x100).IsOk()
        || controlQueue.Front() == nullptr
        || controlQueue.Front()->kind != Http3QuicActionKind::CloseConnection
        || !controlScheduler.AcceptFront()) {
        return 86;
    }
    if (!controlQueue.Empty()
        || controlQueue.Submissions() != 6
        || controlQueue.Refusals() != 0) {
        return 87;
    }

    std::cout
        << "LikesProgram HTTP/3 caller-owned action queue check passed\n";
    return 0;
}
