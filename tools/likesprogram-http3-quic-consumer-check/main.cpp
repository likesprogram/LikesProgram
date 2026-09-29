#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>
#include <LikesProgram/Http/Http3QuicRequestStreamBridge.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {
    struct QueuedReceiveCreditFrame {
        LikesProgram::Http::Http3QuicActionKind actionKind =
            LikesProgram::Http::Http3QuicActionKind::ConnectionReceiveCredit;
        std::uint64_t actionId = 0;
        std::uint64_t streamId = 0;
        std::uint64_t limit = 0;
        std::vector<std::uint8_t> bytes;
    };

    class ReceiveCreditWireSink final
        : public LikesProgram::Http::Http3QuicActionSink {
    public:
        void Submit(
            const LikesProgram::Http::Http3QuicAction& action) noexcept override {
            ++m_submissions;
            if (m_pending.has_value()
                || action.actionId == 0
                || action.flowCredit == 0
                || action.flowCredit > LikesProgram::Quic::kQuicVarIntMaximum
                || action.errorCode != 0
                || !action.payload.empty()) {
                ++m_failures;
                return;
            }

            LikesProgram::Quic::QuicFlowControlFrame frame;
            if (action.kind
                == LikesProgram::Http::Http3QuicActionKind::ConnectionReceiveCredit) {
                if (action.streamId != 0) {
                    ++m_failures;
                    return;
                }
                frame.kind = LikesProgram::Quic::QuicFlowControlFrameKind::MaxData;
            }
            else if (action.kind
                == LikesProgram::Http::Http3QuicActionKind::StreamReceiveCredit) {
                if (action.streamId > LikesProgram::Quic::kQuicVarIntMaximum
                    || action.streamId % 4 != 0) {
                    ++m_failures;
                    return;
                }
                frame.kind =
                    LikesProgram::Quic::QuicFlowControlFrameKind::MaxStreamData;
                frame.streamId = action.streamId;
            }
            else {
                ++m_failures;
                return;
            }
            frame.limit = action.flowCredit;

            try {
                auto encoded = LikesProgram::Quic::BuildQuicFlowControlFrame(frame);
                if (!encoded.IsOk()) {
                    ++m_failures;
                    return;
                }
                m_pending.emplace(QueuedReceiveCreditFrame{ action.kind,
                    action.actionId, action.streamId, action.flowCredit,
                    std::move(encoded.Value()) });
            }
            catch (...) {
                ++m_failures;
            }
        }

        bool HasPending() const noexcept { return m_pending.has_value(); }
        std::size_t Submissions() const noexcept { return m_submissions; }
        std::size_t Failures() const noexcept { return m_failures; }

        std::optional<QueuedReceiveCreditFrame> Take() {
            auto result = std::move(m_pending);
            m_pending.reset();
            return result;
        }

    private:
        std::optional<QueuedReceiveCreditFrame> m_pending;
        std::size_t m_submissions = 0;
        std::size_t m_failures = 0;
    };

    class DrainRefreshObserver final
        : public LikesProgram::Http::HttpBodyDrainObserver {
    public:
        void Observe(
            const LikesProgram::Http::HttpBodyDrainEvent& event) noexcept override {
            ++events;
            if (bridge == nullptr || adapter == nullptr
                || event.streamId != bridge->Snapshot().stream.streamId) {
                ok = false;
                return;
            }
            if (connection == nullptr) {
                const auto refreshed = bridge->RefreshStreamReceiveCredit(*adapter);
                ok = ok && refreshed.IsOk() && refreshed.Value();
                return;
            }
            const auto observed = connection->ObserveRequestStream(bridge->Snapshot());
            if (!observed.IsOk() || !observed.Value()) {
                ok = false;
                return;
            }
            const auto refreshed = connection->RefreshConnectionReceiveCredit(*adapter);
            ok = ok && refreshed.IsOk() && refreshed.Value();
        }

        LikesProgram::Http::Http3QuicRequestStreamBridge* bridge = nullptr;
        LikesProgram::Http::Http3QuicAdapter* adapter = nullptr;
        LikesProgram::Http::Http3ConnectionReceiveCreditCoordinator* connection = nullptr;
        std::size_t events = 0;
        bool ok = true;
    };

    bool MatchesFrame(
        const QueuedReceiveCreditFrame& queued,
        LikesProgram::Http::Http3QuicActionKind actionKind,
        LikesProgram::Quic::QuicFlowControlFrameKind frameKind,
        std::uint64_t streamId,
        std::uint64_t limit,
        const std::vector<std::uint8_t>& exactBytes) {
        if (queued.actionKind != actionKind
            || queued.actionId == 0
            || queued.streamId != streamId
            || queued.limit != limit
            || queued.bytes != exactBytes) {
            return false;
        }
        const auto parsed = LikesProgram::Quic::ParseQuicFlowControlFrame(
            queued.bytes.data(), queued.bytes.size());
        return parsed.IsOk()
            && parsed.Value().kind == frameKind
            && parsed.Value().streamId == streamId
            && parsed.Value().limit == limit
            && parsed.Value().consumedBytes == queued.bytes.size();
    }

    bool Feedback(
        LikesProgram::Http::Http3QuicAdapter& adapter,
        LikesProgram::Http::Http3QuicTransportFeedbackKind kind,
        std::uint64_t actionId,
        std::uint64_t errorCode = 0) {
        return adapter.FeedTransportFeedback(
            { kind, actionId, 0, errorCode }).IsOk();
    }
}

int main() {
    using LikesProgram::Http::Http3QuicActionKind;
    using LikesProgram::Http::Http3QuicEventKind;
    using LikesProgram::Http::Http3QuicTransportFeedbackKind;
    using LikesProgram::Quic::QuicFlowControlFrameKind;

    ReceiveCreditWireSink wire;
    LikesProgram::Http::Http3QuicAction invalid;
    invalid.kind = Http3QuicActionKind::StreamData;
    invalid.actionId = 1;
    invalid.flowCredit = 1;
    wire.Submit(invalid);
    invalid.kind = Http3QuicActionKind::ConnectionReceiveCredit;
    invalid.actionId = 0;
    wire.Submit(invalid);
    invalid.actionId = 2;
    invalid.flowCredit = 0;
    wire.Submit(invalid);
    invalid.flowCredit = LikesProgram::Quic::kQuicVarIntMaximum + 1;
    wire.Submit(invalid);
    invalid.flowCredit = 1;
    invalid.streamId = 4;
    wire.Submit(invalid);
    invalid.kind = Http3QuicActionKind::StreamReceiveCredit;
    invalid.streamId = 1;
    wire.Submit(invalid);
    if (wire.HasPending() || wire.Failures() != 6 || wire.Submissions() != 6) {
        return 1;
    }

    LikesProgram::Http::Http3QuicAdapter adapter(&wire);
    if (!adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !adapter.Feed({ Http3QuicEventKind::StreamData,
            0, 0, { 'x' } }).IsOk()
        || !adapter.UpdateConnectionReceiveCredit(64).IsOk()) {
        return 2;
    }
    auto connection64 = wire.Take();
    if (!connection64.has_value()
        || !MatchesFrame(*connection64,
            Http3QuicActionKind::ConnectionReceiveCredit,
            QuicFlowControlFrameKind::MaxData, 0, 64,
            { 0x10, 0x40, 0x40 })
        || !Feedback(adapter, Http3QuicTransportFeedbackKind::ActionAccepted,
            connection64->actionId)
        || adapter.ReceiveCredit(0).Value().connectionCredit != 64) {
        return 3;
    }

    if (!adapter.UpdateStreamReceiveCredit(0, 32).IsOk()) return 4;
    auto rejected32 = wire.Take();
    if (!rejected32.has_value()
        || !MatchesFrame(*rejected32,
            Http3QuicActionKind::StreamReceiveCredit,
            QuicFlowControlFrameKind::MaxStreamData, 0, 32,
            { 0x11, 0x00, 0x20 })
        || !Feedback(adapter, Http3QuicTransportFeedbackKind::ActionRejected,
            rejected32->actionId, 0x71)
        || adapter.ReceiveCredit(0).Value().streamCreditSet
        || !adapter.UpdateStreamReceiveCredit(0, 32).IsOk()) {
        return 5;
    }
    auto retried32 = wire.Take();
    if (!retried32.has_value()
        || retried32->actionId <= rejected32->actionId
        || retried32->bytes != rejected32->bytes
        || !MatchesFrame(*retried32,
            Http3QuicActionKind::StreamReceiveCredit,
            QuicFlowControlFrameKind::MaxStreamData, 0, 32,
            { 0x11, 0x00, 0x20 })
        || !Feedback(adapter, Http3QuicTransportFeedbackKind::ActionAccepted,
            retried32->actionId)
        || adapter.ReceiveCredit(0).Value().streamCredit != 32) {
        return 6;
    }

    LikesProgram::Http::HttpBodySink sink({ 8, 2, 8 });
    LikesProgram::Http::Http3QuicRequestStreamBridge bridge(0);
    DrainRefreshObserver drain;
    drain.bridge = &bridge;
    drain.adapter = &adapter;
    const auto headers = LikesProgram::Http::BuildHttp3Frame({
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Headers),
        { 0x01 } });
    const auto data = LikesProgram::Http::BuildHttp3Frame({
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data),
        { 'a', 'b' } });
    if (!headers.IsOk() || !data.IsOk()
        || !bridge.AttachBodySink(&sink).IsOk()
        || !bridge.ConfigureStreamReceiveCredit(32).IsOk()
        || !sink.AttachDrainObserver(&drain, 0).IsOk()
        || !bridge.Feed({ Http3QuicEventKind::StreamData,
            0, 0, headers.Value() }).IsOk()
        || !bridge.Feed({ Http3QuicEventKind::StreamData,
            0, 0, data.Value() }).IsOk()
        || !sink.Pull(1).IsOk()
        || !drain.ok || drain.events != 1) {
        return 7;
    }
    auto stream33 = wire.Take();
    if (!stream33.has_value()
        || !MatchesFrame(*stream33,
            Http3QuicActionKind::StreamReceiveCredit,
            QuicFlowControlFrameKind::MaxStreamData, 0, 33,
            { 0x11, 0x00, 0x21 })
        || !Feedback(adapter, Http3QuicTransportFeedbackKind::ActionAccepted,
            stream33->actionId)) {
        return 8;
    }

    LikesProgram::Http::Http3ConnectionReceiveCreditCoordinator connection;
    if (!connection.Configure(64).IsOk()
        || !connection.RegisterRequestStream(bridge.Snapshot()).IsOk()) {
        return 9;
    }
    drain.connection = &connection;
    if (!sink.Pull(1).IsOk() || !drain.ok || drain.events != 2) return 10;
    auto connection65 = wire.Take();
    if (!connection65.has_value()
        || !MatchesFrame(*connection65,
            Http3QuicActionKind::ConnectionReceiveCredit,
            QuicFlowControlFrameKind::MaxData, 0, 65,
            { 0x10, 0x40, 0x41 })
        || !Feedback(adapter, Http3QuicTransportFeedbackKind::ActionAccepted,
            connection65->actionId)
        || adapter.ReceiveCredit(0).Value().connectionCredit != 65
        || wire.HasPending()
        || wire.Failures() != 6
        || wire.Submissions() != 11) {
        return 11;
    }

    std::cout << "LikesProgram HTTP/3 QUIC wire mapping check passed\n";
    return 0;
}
