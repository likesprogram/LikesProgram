#include <LikesProgram/Core/Status.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>
#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {
    using LikesProgram::Http::Http3QuicAction;
    using LikesProgram::Http::Http3QuicActionKind;
    using LikesProgram::Http::Http3QuicAdapter;
    using LikesProgram::Http::Http3QuicAdapterState;
    using LikesProgram::Http::Http3QuicTransportFeedbackKind;

    constexpr std::uint64_t kMaximumQuicVarInt =
        (std::uint64_t{ 1 } << 62) - 1;

    struct DeferredCloseRejection {
        bool available = false;
        std::uint64_t actionId = 0;
    };

    class CloseActionQueue final
        : public LikesProgram::Http::Http3QuicActionSink {
    public:
        void Submit(const Http3QuicAction& action) noexcept override {
            ++m_submitDepth;
            if (m_submitDepth > m_maxSubmitDepth) {
                m_maxSubmitDepth = m_submitDepth;
            }
            ++m_submissions;

            if (!m_writable || m_front.has_value() || !IsValid(action)) {
                Defer(action.actionId);
                --m_submitDepth;
                return;
            }

            try {
                m_front = action;
            }
            catch (...) {
                Defer(action.actionId);
            }
            --m_submitDepth;
        }

        void SetWritable(bool writable) noexcept {
            m_writable = writable;
        }

        bool Empty() const noexcept {
            return !m_front.has_value();
        }

        const Http3QuicAction* Front() const noexcept {
            return m_front.has_value() ? &*m_front : nullptr;
        }

        void PopFront() noexcept {
            m_front.reset();
        }

        DeferredCloseRejection TakeDeferredRejection() noexcept {
            const auto result = m_deferred;
            m_deferred = {};
            return result;
        }

        bool HasDeferredRejection() const noexcept {
            return m_deferred.available;
        }

        std::size_t Submissions() const noexcept {
            return m_submissions;
        }

        std::size_t Refusals() const noexcept {
            return m_refusals;
        }

        std::size_t LostRefusals() const noexcept {
            return m_lostRefusals;
        }

        std::size_t MaxSubmitDepth() const noexcept {
            return m_maxSubmitDepth;
        }

        void Reset() noexcept {
            m_front.reset();
            m_deferred = {};
            m_writable = true;
            m_submitDepth = 0;
        }

    private:
        static bool IsValid(const Http3QuicAction& action) noexcept {
            return action.kind == Http3QuicActionKind::CloseConnection
                && action.actionId != 0
                && action.streamId == 0
                && action.errorCode <= kMaximumQuicVarInt
                && action.payload.empty()
                && action.flowCredit == 0;
        }

        void Defer(std::uint64_t actionId) noexcept {
            ++m_refusals;
            if (m_deferred.available) {
                ++m_lostRefusals;
                return;
            }
            m_deferred = { true, actionId };
        }

        bool m_writable = true;
        std::optional<Http3QuicAction> m_front;
        DeferredCloseRejection m_deferred;
        std::size_t m_submissions = 0;
        std::size_t m_refusals = 0;
        std::size_t m_lostRefusals = 0;
        std::size_t m_submitDepth = 0;
        std::size_t m_maxSubmitDepth = 0;
    };

    struct DeadlineWakeSnapshot {
        bool armed = false;
        std::uint64_t generation = 0;
        LikesProgram::Time::SteadyTimePoint due{};
    };

    class CallerDeadlineWakeDriver {
    public:
        CallerDeadlineWakeDriver(
            Http3QuicAdapter& adapter,
            CloseActionQueue& queue) noexcept
            : m_adapter(adapter), m_queue(queue) { }

        bool Arm(LikesProgram::Time::Deadline deadline,
            std::uint64_t errorCode) {
            if (m_adapter.State() != Http3QuicAdapterState::Ready
                || !m_queue.Empty()
                || m_queue.HasDeferredRejection()
                || (deadline.HasDeadline()
                    && errorCode > kMaximumQuicVarInt)) {
                return false;
            }

            const auto set = m_adapter.SetDeadline(deadline);
            if (!set.IsOk()
                && set.GetStatus().Code()
                    != LikesProgram::StatusCode::DeadlineExceeded) {
                return false;
            }

            m_generation = NextGeneration(m_generation);
            m_armed = deadline.HasDeadline();
            m_due = deadline.TimePoint();
            m_errorCode = errorCode;
            return true;
        }

        DeadlineWakeSnapshot Wake() const noexcept {
            return { m_armed, m_generation, m_due };
        }

        bool OnWake(std::uint64_t generation) {
            if (!m_armed || generation != m_generation) return true;
            if (m_adapter.State() == Http3QuicAdapterState::Closing) {
                return m_queue.Front() != nullptr;
            }
            if (m_adapter.State() != Http3QuicAdapterState::Ready) {
                return false;
            }

            const auto checked = m_adapter.CheckDeadline();
            if (checked.IsOk()) return true;
            if (checked.GetStatus().Code()
                != LikesProgram::StatusCode::DeadlineExceeded) {
                return false;
            }

            if (!m_adapter.CloseExpiredDeadline(m_errorCode).IsOk()) {
                return false;
            }
            return ConsumeDeferredRejection();
        }

        bool OnWritable() {
            return OnWake(m_generation);
        }

        bool RejectFront(std::uint64_t transportErrorCode) {
            const auto* action = m_queue.Front();
            if (action == nullptr) return false;
            const auto actionId = action->actionId;
            if (!m_adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    actionId, 0, transportErrorCode }).IsOk()) {
                return false;
            }
            m_queue.PopFront();
            return OnWritable();
        }

        bool AcceptFront() {
            const auto* action = m_queue.Front();
            if (action == nullptr) return false;
            const auto actionId = action->actionId;
            if (!m_adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    actionId, 0, 0 }).IsOk()) {
                return false;
            }
            m_queue.PopFront();
            m_armed = false;
            return true;
        }

        std::uint64_t LastDeferredRejectedActionId() const noexcept {
            return m_lastDeferredRejectedActionId;
        }

        void Reset() noexcept {
            m_adapter.Reset();
            m_queue.Reset();
            m_generation = NextGeneration(m_generation);
            m_armed = false;
            m_due = {};
            m_errorCode = 0;
        }

    private:
        static std::uint64_t NextGeneration(
            std::uint64_t generation) noexcept {
            return generation == kMaximumQuicVarInt
                ? std::uint64_t{ 1 } : generation + 1;
        }

        bool ConsumeDeferredRejection() {
            const auto deferred = m_queue.TakeDeferredRejection();
            if (!deferred.available) return true;
            m_lastDeferredRejectedActionId = deferred.actionId;
            return deferred.actionId != 0
                && m_adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    deferred.actionId, 0, 0 }).IsOk();
        }

        Http3QuicAdapter& m_adapter;
        CloseActionQueue& m_queue;
        LikesProgram::Time::SteadyTimePoint m_due{};
        std::uint64_t m_generation = 0;
        std::uint64_t m_errorCode = 0;
        std::uint64_t m_lastDeferredRejectedActionId = 0;
        bool m_armed = false;
    };

    bool IsStatus(const LikesProgram::Result<void>& result,
        LikesProgram::StatusCode code) {
        return !result.IsOk() && result.GetStatus().Code() == code;
    }
}

int main() {
    using LikesProgram::Http::Http3ErrorCode;
    using LikesProgram::Http::Http3QuicEventKind;
    using LikesProgram::Time::Deadline;

    CloseActionQueue queue;
    Http3QuicAdapter adapter(&queue);
    CallerDeadlineWakeDriver driver(adapter, queue);
    const auto deadlineError = static_cast<std::uint64_t>(
        Http3ErrorCode::RequestCancelled);

    if (driver.Arm(Deadline::FromNow(std::chrono::hours(1)), deadlineError)) {
        return 1;
    }
    if (!adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()) {
        return 2;
    }

    if (!driver.Arm(Deadline::FromNow(std::chrono::hours(1)), deadlineError)) {
        return 3;
    }
    const auto firstWake = driver.Wake();
    if (!firstWake.armed || firstWake.generation == 0
        || firstWake.due <= LikesProgram::Time::Clock::Now()
        || !driver.OnWake(firstWake.generation)
        || queue.Submissions() != 0) {
        return 4;
    }

    if (!driver.Arm(Deadline::FromNow(std::chrono::hours(2)), deadlineError)) {
        return 5;
    }
    const auto replacementWake = driver.Wake();
    if (replacementWake.generation == firstWake.generation
        || !driver.OnWake(firstWake.generation)
        || queue.Submissions() != 0) {
        return 6;
    }
    if (!driver.Arm(Deadline::Infinite(), 0)
        || driver.Wake().armed
        || adapter.HasDeadline()
        || !driver.OnWake(replacementWake.generation)
        || queue.Submissions() != 0) {
        return 7;
    }

    const auto cancelledWake = driver.Wake();
    if (driver.Arm(Deadline::FromNow(std::chrono::hours(1)),
            kMaximumQuicVarInt + 1)
        || driver.Wake().generation != cancelledWake.generation
        || adapter.HasDeadline()) {
        return 8;
    }

    queue.SetWritable(false);
    if (!driver.Arm(Deadline::FromNow(
            LikesProgram::Time::Duration::zero()), deadlineError)) {
        return 9;
    }
    const auto expiredWake = driver.Wake();
    if (!driver.OnWake(expiredWake.generation)
        || !driver.Wake().armed
        || !adapter.DeadlineExpired()
        || adapter.State() != Http3QuicAdapterState::Ready
        || adapter.Snapshot().pendingTerminalActions != 0
        || !queue.Empty()
        || queue.Refusals() != 1
        || queue.LostRefusals() != 0
        || driver.LastDeferredRejectedActionId() == 0) {
        return 10;
    }

    const auto capacityRejectedId = driver.LastDeferredRejectedActionId();
    queue.SetWritable(true);
    if (!driver.OnWritable()
        || queue.Front() == nullptr
        || queue.Front()->actionId <= capacityRejectedId
        || queue.Front()->kind != Http3QuicActionKind::CloseConnection
        || queue.Front()->errorCode != deadlineError
        || adapter.State() != Http3QuicAdapterState::Closing
        || adapter.Snapshot().pendingTerminalActions != 1) {
        return 11;
    }

    const auto firstQueuedId = queue.Front()->actionId;
    if (!IsStatus(adapter.FeedTransportFeedback({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            firstQueuedId + 100, 0, 0 }),
            LikesProgram::StatusCode::FailedPrecondition)
        || queue.Front() == nullptr
        || queue.Front()->actionId != firstQueuedId
        || adapter.Snapshot().pendingTerminalActions != 1) {
        return 12;
    }
    if (driver.Arm(Deadline::FromNow(std::chrono::hours(1)), deadlineError)
        || driver.Wake().generation != expiredWake.generation) {
        return 13;
    }

    if (!driver.RejectFront(0x90)
        || queue.Front() == nullptr
        || queue.Front()->actionId <= firstQueuedId
        || adapter.State() != Http3QuicAdapterState::Closing
        || adapter.Snapshot().pendingTerminalActions != 1
        || !driver.Wake().armed) {
        return 14;
    }
    const auto retriedId = queue.Front()->actionId;
    if (!driver.AcceptFront()
        || !queue.Empty()
        || driver.Wake().armed
        || adapter.State() != Http3QuicAdapterState::Closing
        || adapter.Snapshot().pendingTerminalActions != 0
        || !driver.OnWake(expiredWake.generation)
        || queue.Submissions() != 3) {
        return 15;
    }

    driver.Reset();
    const auto resetWake = driver.Wake();
    if (resetWake.armed
        || resetWake.generation == expiredWake.generation
        || adapter.State() != Http3QuicAdapterState::AwaitingHandshake
        || adapter.HasDeadline()
        || !queue.Empty()
        || !driver.OnWake(expiredWake.generation)) {
        return 16;
    }
    if (!adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !driver.Arm(Deadline::FromNow(
            LikesProgram::Time::Duration::zero()), deadlineError)) {
        return 17;
    }
    const auto resetPendingWake = driver.Wake();
    if (!driver.OnWake(resetPendingWake.generation)
        || queue.Front() == nullptr
        || queue.Front()->actionId != 1
        || queue.Submissions() != 4) {
        return 18;
    }
    driver.Reset();
    if (!queue.Empty()
        || adapter.State() != Http3QuicAdapterState::AwaitingHandshake
        || adapter.Snapshot().pendingTerminalActions != 0
        || !driver.OnWake(resetPendingWake.generation)
        || queue.Submissions() != 4
        || queue.MaxSubmitDepth() != 1) {
        return 19;
    }

    std::cout << "passed=true"
              << " submissions=" << queue.Submissions()
              << " refusals=" << queue.Refusals()
              << " capacity_rejected_id=" << capacityRejectedId
              << " transport_retried_id=" << retriedId
              << " max_submit_depth=" << queue.MaxSubmitDepth()
              << '\n';
    return 0;
}
