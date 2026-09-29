#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
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

    struct DispatchObservation {
        QuicFrameType kind = QuicFrameType::Unknown;
        std::size_t offset = 0;
        std::size_t consumedBytes = 0;
        bool mapped = false;

        bool operator==(const DispatchObservation&) const = default;
    };

    struct MappingSnapshot {
        std::vector<DispatchObservation> observations;
        std::vector<QuicStreamEvent> events;
        std::vector<QuicActionMessage> actions;
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

    bool SamePayload(const LikesProgram::Net::Buffer& buffer,
        std::span<const std::uint8_t> expected) {
        return buffer.ReadableBytes() == expected.size()
            && std::equal(expected.begin(), expected.end(), buffer.Peek());
    }

    bool DispatchAndMap(std::span<const std::uint8_t> bytes,
        const Address& peer, MappingSnapshot& snapshot) {
        MappingSnapshot pending;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto type = ParseQuicFrameType(
                bytes.data() + offset, bytes.size() - offset);
            if (!type.IsOk()) return false;

            std::size_t consumedBytes = 0;
            bool mapped = false;
            switch (type.Value().kind) {
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
                mapped = true;
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
                mapped = true;
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
                mapped = true;
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
                mapped = true;
                break;
            }
            default:
                return false;
            }

            if (consumedBytes < type.Value().encodedBytes
                || consumedBytes > bytes.size() - offset) {
                return false;
            }
            pending.observations.push_back({
                type.Value().kind, offset, consumedBytes, mapped });
            offset += consumedBytes;
        }
        snapshot = std::move(pending);
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

    std::vector<std::uint8_t> sequence;
    Require(Append(sequence, BuildQuicPingFrame()), "PING should build");
    Require(Append(sequence, BuildQuicStreamFrame(stream)),
        "STREAM should build");
    Require(Append(sequence, BuildQuicStreamFrame(finStream)),
        "FIN STREAM should build");
    Require(Append(sequence, BuildQuicResetStreamFrame(reset)),
        "RESET_STREAM should build");
    Require(Append(sequence, BuildQuicStopSendingFrame(stop)),
        "STOP_SENDING should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(transportClose)),
        "transport close should build");
    Require(Append(sequence, BuildQuicConnectionCloseFrame(applicationClose)),
        "application close should build");

    MappingSnapshot snapshot;
    Require(DispatchAndMap(sequence, peer, snapshot),
        "mixed sequence should map completely");
    Require(snapshot.observations.size() == 7
        && snapshot.events.size() == 4
        && snapshot.actions.size() == 2,
        "all frame observations should map to the expected vectors");
    Require(snapshot.observations[0].kind == QuicFrameType::Ping
        && !snapshot.observations[0].mapped
        && snapshot.observations[1].mapped
        && snapshot.observations[2].mapped
        && snapshot.observations[5].mapped
        && snapshot.observations[6].mapped,
        "mixed ordering should preserve mapped and unmapped frames");
    for (std::size_t index = 1; index < snapshot.observations.size(); ++index) {
        Require(snapshot.observations[index - 1].offset
                + snapshot.observations[index - 1].consumedBytes
            == snapshot.observations[index].offset,
            "frame coordinates should be contiguous");
    }
    Require(snapshot.observations.back().offset
            + snapshot.observations.back().consumedBytes == sequence.size(),
        "trailing boundary should be exact");

    Require(snapshot.events[0].kind == QuicEventKind::StreamData
        && snapshot.events[0].streamId == 4
        && snapshot.events[0].peer.Ip() == peer.Ip()
        && snapshot.events[0].peer.Port() == peer.Port()
        && SamePayload(snapshot.events[0].payload, data),
        "STREAM data event should preserve peer and payload");
    Require(snapshot.events[1].kind == QuicEventKind::StreamFin
        && SamePayload(snapshot.events[1].payload, finData),
        "FIN event should preserve final payload");
    Require(snapshot.events[2].kind == QuicEventKind::ConnectionClose
        && snapshot.events[2].errorCode == transportClose.errorCode
        && !snapshot.events[2].applicationError
        && SamePayload(snapshot.events[2].payload, transportReason),
        "transport close event should preserve error and reason");
    Require(snapshot.events[3].applicationError
        && snapshot.events[3].errorCode == applicationClose.errorCode
        && SamePayload(snapshot.events[3].payload, applicationReason),
        "application close event should preserve error and reason");
    Require(snapshot.actions[0].kind == QuicActionKind::ResetStream
        && snapshot.actions[0].actionId == 0
        && snapshot.actions[0].streamId == reset.streamId
        && snapshot.actions[0].errorCode == reset.applicationErrorCode
        && snapshot.actions[0].payload.ReadableBytes() > 0,
        "RESET action should keep id external and payload complete");
    Require(snapshot.actions[1].kind == QuicActionKind::StopSending
        && snapshot.actions[1].actionId == 0
        && snapshot.actions[1].streamId == stop.streamId
        && snapshot.actions[1].errorCode == stop.applicationErrorCode
        && snapshot.actions[1].payload.ReadableBytes() > 0,
        "STOP action should keep id external and payload complete");

    const auto baseline = snapshot;
    auto unknown = sequence;
    unknown.insert(unknown.begin() + 1, 0x2A);
    Require(!DispatchAndMap(unknown, peer, snapshot)
        && snapshot.observations == baseline.observations
        && snapshot.events.size() == baseline.events.size()
        && snapshot.actions.size() == baseline.actions.size(),
        "unknown extension should reject atomically");

    auto truncated = sequence;
    truncated.pop_back();
    Require(!DispatchAndMap(truncated, peer, snapshot)
        && snapshot.observations == baseline.observations
        && snapshot.events.size() == baseline.events.size()
        && snapshot.actions.size() == baseline.actions.size(),
        "truncated close should reject atomically");

    const std::array<std::uint8_t, 1> malformedType{ 0x40 };
    Require(!DispatchAndMap(malformedType, peer, snapshot)
        && snapshot.observations == baseline.observations
        && snapshot.events.size() == baseline.events.size()
        && snapshot.actions.size() == baseline.actions.size(),
        "malformed type should reject atomically");

    const QuicStreamFrameBuildOptions lengthless{
        4, 0, true, false, false, data };
    std::vector<std::uint8_t> lengthlessBytes;
    Require(Append(lengthlessBytes, BuildQuicStreamFrame(lengthless)),
        "lengthless STREAM should build");
    Require(!DispatchAndMap(lengthlessBytes, peer, snapshot)
        && snapshot.observations == baseline.observations
        && snapshot.events.size() == baseline.events.size()
        && snapshot.actions.size() == baseline.actions.size(),
        "lengthless STREAM should not be mapped as a bounded sequence");

    std::cout << "passed=true"
              << " sequence_roundtrip=true"
              << " event_mapping=true"
              << " action_mapping=true"
              << " payload_coordinates=true"
              << " close_variants=true"
              << " action_id_external=true"
              << " unknown_rejected=true"
              << " truncated_rejected=true"
              << " malformed_rejected=true"
              << " lengthless_rejected=true"
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
