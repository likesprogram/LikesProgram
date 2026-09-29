#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicStreamControlAction.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamEventMapping.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    struct MappedSequence {
        std::vector<QuicStreamEvent> events;
        std::vector<QuicActionMessage> actions;
        std::size_t frameCount = 0;
    };

    struct ObservedEvent {
        QuicEventKind kind = QuicEventKind::HandshakeComplete;
        std::uint64_t streamId = 0;
        std::uint64_t errorCode = 0;
        bool applicationError = false;
        std::vector<std::uint8_t> payload;
    };

    class RecordingObserver final : public QuicEventObserver {
    public:
        void Observe(QuicStreamEvent&& event) noexcept override {
            ObservedEvent observed;
            observed.kind = event.kind;
            observed.streamId = event.streamId;
            observed.errorCode = event.errorCode;
            observed.applicationError = event.applicationError;
            const auto* bytes = event.payload.Peek();
            observed.payload.assign(bytes, bytes + event.payload.ReadableBytes());
            events.push_back(std::move(observed));
        }

        std::vector<ObservedEvent> events;
    };

    class RecordingSink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if (rejectNext) {
                rejectNext = false;
                ++rejections;
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::TransportUnavailable, 0 };
            }
            action.actionId = nextId++;
            accepted.push_back(std::move(action));
            return { QuicActionDeliveryState::Accepted,
                QuicActionDeliveryError::None, accepted.back().actionId };
        }

        bool rejectNext = false;
        std::uint64_t nextId = 1;
        std::size_t rejections = 0;
        std::vector<QuicActionMessage> accepted;
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool Append(std::vector<std::uint8_t>& destination,
        const LikesProgram::Result<std::vector<std::uint8_t>>& encoded) {
        if (!encoded.IsOk()) return false;
        const auto& bytes = encoded.Value();
        destination.insert(destination.end(), bytes.begin(), bytes.end());
        return true;
    }

    bool MapSequence(std::span<const std::uint8_t> bytes,
        const Address& peer, MappedSequence& output) {
        MappedSequence pending;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto type = ParseQuicFrameType(
                bytes.data() + offset, bytes.size() - offset);
            if (!type.IsOk()) return false;

            std::size_t consumedBytes = 0;
            switch (type.Value().kind) {
            case QuicFrameType::Padding:
                if (type.Value().value != 0) return false;
                consumedBytes = type.Value().encodedBytes;
                break;
            case QuicFrameType::Ping: {
                const auto parsed = ParseQuicPingFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Stream: {
                if ((type.Value().value & 0x02) == 0) return false;
                const auto parsed = ParseQuicStreamFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk() || !parsed.Value().hasLength) return false;
                const auto event = MapQuicStreamFrameToEvent(
                    parsed.Value(), peer);
                if (!event.IsOk()) return false;
                pending.events.push_back(event.Value());
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::ResetStream: {
                const auto parsed = ParseQuicResetStreamFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                const auto action = BuildQuicResetStreamAction(parsed.Value());
                if (!action.IsOk()) return false;
                pending.actions.push_back(action.Value());
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::StopSending: {
                const auto parsed = ParseQuicStopSendingFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                const auto action = BuildQuicStopSendingAction(parsed.Value());
                if (!action.IsOk()) return false;
                pending.actions.push_back(action.Value());
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::ConnectionClose: {
                const auto parsed = ParseQuicConnectionCloseFrame(
                    bytes.data() + offset, bytes.size() - offset);
                if (!parsed.IsOk()) return false;
                const auto event = MapQuicConnectionCloseFrameToEvent(
                    parsed.Value(), peer);
                if (!event.IsOk()) return false;
                pending.events.push_back(event.Value());
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            default:
                return false;
            }

            if (consumedBytes < type.Value().encodedBytes
                || consumedBytes > bytes.size() - offset) {
                return false;
            }
            ++pending.frameCount;
            offset += consumedBytes;
        }
        output = std::move(pending);
        return true;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const Address peer("127.0.0.1", 4433);
    const std::array<std::uint8_t, 2> data{ 0x11, 0x12 };
    const std::array<std::uint8_t, 1> finData{ 0x13 };
    const std::array<std::uint8_t, 3> transportReason{ 'b', 'y', 'e' };
    const std::array<std::uint8_t, 2> applicationReason{ 'o', 'k' };
    const QuicStreamFrameBuildOptions stream{
        4, 0, true, true, false, data };
    const QuicStreamFrameBuildOptions finStream{
        4, 2, true, true, true, finData };
    const QuicResetStreamFrame reset{ 4, 7, 3, 0 };
    const QuicStopSendingFrame stop{ 4, 8, 0 };
    const QuicConnectionCloseFrame transportClose{
        false, 0x12, 0x08, transportReason, 0 };
    const QuicConnectionCloseFrame applicationClose{
        true, 0x21, 0, applicationReason, 0 };

    std::vector<std::uint8_t> sequence{ 0x00, 0x00 };
    Require(Append(sequence, BuildQuicPingFrame()), "PING should build");
    Require(Append(sequence, BuildQuicStreamFrame(stream)),
        "STREAM should build");
    Require(Append(sequence, BuildQuicStreamFrame(finStream)),
        "FIN STREAM should build");
    Require(Append(sequence, BuildQuicResetStreamFrame(reset)),
        "RESET_STREAM should build");
    sequence.push_back(0x00);
    Require(Append(sequence, BuildQuicStopSendingFrame(stop)),
        "STOP_SENDING should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(transportClose)),
        "transport close should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(applicationClose)),
        "application close should build");
    sequence.push_back(0x00);

    MappedSequence mapped;
    Require(MapSequence(sequence, peer, mapped), "sequence should map");
    Require(mapped.frameCount == 11 && mapped.events.size() == 4
        && mapped.actions.size() == 2,
        "mapping should preserve the padded sequence cardinality");

    RecordingObserver observer;
    for (auto& event : mapped.events) observer.Observe(std::move(event));
    Require(observer.events.size() == 4
        && observer.events[0].kind == QuicEventKind::StreamData
        && observer.events[1].kind == QuicEventKind::StreamFin
        && observer.events[2].kind == QuicEventKind::ConnectionClose
        && observer.events[3].kind == QuicEventKind::ConnectionClose
        && observer.events[2].errorCode == transportClose.errorCode
        && !observer.events[2].applicationError
        && observer.events[3].applicationError,
        "observer must retain mapped event order and close variants");
    Require(observer.events[0].payload
            == std::vector<std::uint8_t>(data.begin(), data.end())
        && observer.events[1].payload
            == std::vector<std::uint8_t>(finData.begin(), finData.end())
        && observer.events[2].payload
            == std::vector<std::uint8_t>(transportReason.begin(), transportReason.end())
        && observer.events[3].payload
            == std::vector<std::uint8_t>(applicationReason.begin(), applicationReason.end()),
        "observer must copy event payload ownership");

    RecordingSink sink;
    sink.rejectNext = true;
    const auto firstAction = mapped.actions[0];
    const auto rejected = sink.Submit(QuicActionMessage(firstAction));
    Require(rejected.state == QuicActionDeliveryState::Rejected
        && rejected.error == QuicActionDeliveryError::TransportUnavailable
        && rejected.actionId == 0 && sink.rejections == 1,
        "first action rejection must remain external feedback");
    const auto retry = sink.Submit(QuicActionMessage(firstAction));
    const auto second = sink.Submit(QuicActionMessage(mapped.actions[1]));
    Require(retry.state == QuicActionDeliveryState::Accepted
        && second.state == QuicActionDeliveryState::Accepted
        && retry.actionId != 0 && second.actionId != 0
        && retry.actionId != second.actionId
        && sink.accepted.size() == 2
        && sink.accepted[0].actionId == retry.actionId
        && sink.accepted[1].actionId == second.actionId,
        "accepted actions must receive fresh non-zero ids after Submit");
    Require(sink.accepted[0].kind == QuicActionKind::ResetStream
        && sink.accepted[1].kind == QuicActionKind::StopSending
        && sink.accepted[0].payload.ReadableBytes() > 0
        && sink.accepted[1].payload.ReadableBytes() > 0,
        "sink must retain mapped action payloads");

    const auto baselineFrames = mapped.frameCount;
    auto malformed = sequence;
    malformed.insert(malformed.begin() + 1, 0x2A);
    Require(!MapSequence(malformed, peer, mapped)
        && mapped.frameCount == baselineFrames
        && mapped.events.size() == observer.events.size()
        && mapped.actions.size() == sink.accepted.size(),
        "failed mapping must preserve prior delivery inputs");

    std::cout << "passed=true"
              << " frame_count=true"
              << " observer_order=true"
              << " payload_copied=true"
              << " close_variants=true"
              << " rejection_feedback=true"
              << " retry_fresh_ids=true"
              << " action_payload_owned=true"
              << " malformed_rejected=true"
              << " state_preserved=true\n";
    return 0;
}

int main() {
    try {
        return Run();
    }
    catch (const std::exception& error) {
        std::cerr << "failed=" << error.what() << '\n';
        return 99;
    }
}
