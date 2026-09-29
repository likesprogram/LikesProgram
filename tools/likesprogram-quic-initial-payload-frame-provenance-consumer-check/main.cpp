#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
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

    struct FrameObservation {
        QuicFrameType kind = QuicFrameType::Unknown;
        std::size_t offset = 0;
        std::size_t consumedBytes = 0;
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool SameBytes(std::span<const std::uint8_t> left,
        std::span<const std::uint8_t> right) {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin());
    }

    bool Append(std::vector<std::uint8_t>& destination,
        const LikesProgram::Result<std::vector<std::uint8_t>>& encoded) {
        if (!encoded.IsOk()) return false;
        const auto& bytes = encoded.Value();
        destination.insert(destination.end(), bytes.begin(), bytes.end());
        return true;
    }

    bool DispatchPayload(std::span<const std::uint8_t> payload,
        std::vector<FrameObservation>& observations) {
        observations.clear();
        std::vector<FrameObservation> pending;
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const auto type = ParseQuicFrameType(
                payload.data() + offset, payload.size() - offset);
            if (!type.IsOk()) return false;

            std::size_t consumedBytes = 0;
            switch (type.Value().kind) {
            case QuicFrameType::Ping: {
                const auto parsed = ParseQuicPingFrame(
                    payload.data() + offset, payload.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Ack: {
                const auto parsed = ParseQuicAckFrame(
                    payload.data() + offset, payload.size() - offset);
                if (!parsed.IsOk()) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            case QuicFrameType::Stream: {
                if ((type.Value().value & 0x02) == 0) return false;
                const auto parsed = ParseQuicStreamFrame(
                    payload.data() + offset, payload.size() - offset);
                if (!parsed.IsOk() || !parsed.Value().hasLength) return false;
                consumedBytes = parsed.Value().consumedBytes;
                break;
            }
            default:
                return false;
            }

            if (consumedBytes < type.Value().encodedBytes
                || consumedBytes > payload.size() - offset) {
                return false;
            }
            pending.push_back({ type.Value().kind, offset, consumedBytes });
            offset += consumedBytes;
        }
        observations = std::move(pending);
        return true;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 2> streamData{ 0xD0, 0xD1 };
    const QuicStreamFrameBuildOptions stream{
        7, 2, true, true, true, streamData };
    const QuicAckFrame ack{
        false, 12, 1, { { 11, 12 } }, 0, 0, 0, 0 };

    std::vector<std::uint8_t> payload;
    Require(Append(payload, BuildQuicPingFrame()), "PING should build");
    Require(Append(payload, BuildQuicAckFrame(ack)), "ACK should build");
    Require(Append(payload, BuildQuicStreamFrame(stream)),
        "STREAM should build");

    const std::array<std::uint8_t, 4> destination{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 3> source{ 5, 6, 7 };
    const std::array<std::uint8_t, 1> token{ 0xA0 };
    const std::array<std::uint8_t, 2> packetNumber{ 0x01, 0x02 };
    const auto packet = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, token,
        packetNumber, payload, {} });
    Require(packet.IsOk(), "Initial packet should build around payload bytes");

    const auto parsed = ParseQuicLongHeader(
        packet.Value().data(), packet.Value().size());
    Require(parsed.IsOk()
        && parsed.Value().type == QuicLongPacketType::Initial
        && parsed.Value().version == 1
        && parsed.Value().packetNumberLength == packetNumber.size()
        && SameBytes(parsed.Value().destinationConnectionId, destination)
        && SameBytes(parsed.Value().sourceConnectionId, source)
        && SameBytes(parsed.Value().token, token)
        && SameBytes(parsed.Value().packetNumber, packetNumber)
        && SameBytes(parsed.Value().payload, payload)
        && parsed.Value().consumedBytes == packet.Value().size(),
        "Initial header must preserve packet and payload coordinates");

    std::vector<FrameObservation> observations;
    Require(DispatchPayload(parsed.Value().payload, observations)
        && observations.size() == 3,
        "parsed Initial payload sequence should dispatch completely");
    const std::array<QuicFrameType, 3> expectedKinds{
        QuicFrameType::Ping, QuicFrameType::Ack, QuicFrameType::Stream };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "payload frame coordinates should remain relative to payload");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == parsed.Value().payload.size(),
        "payload dispatch must consume the exact parsed payload span");

    auto trailingDatagram = packet.Value();
    trailingDatagram.push_back(0xA5);
    const auto parsedTrailing = ParseQuicLongHeader(
        trailingDatagram.data(), trailingDatagram.size());
    Require(parsedTrailing.IsOk()
        && parsedTrailing.Value().consumedBytes + 1 == trailingDatagram.size(),
        "long-header parser must expose an external trailing datagram boundary");

    auto payloadUnknown = payload;
    payloadUnknown.push_back(0x2A);
    const auto unknownPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, token,
        packetNumber, payloadUnknown, {} });
    Require(unknownPacket.IsOk(), "unknown payload packet should build");
    const auto parsedUnknown = ParseQuicLongHeader(
        unknownPacket.Value().data(), unknownPacket.Value().size());
    Require(parsedUnknown.IsOk(), "unknown payload header should still parse");
    Require(!DispatchPayload(parsedUnknown.Value().payload, observations)
        && observations.empty(),
        "unknown payload frame must reject without partial observations");

    auto truncatedPayload = payload;
    truncatedPayload.pop_back();
    const auto truncatedPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, token,
        packetNumber, truncatedPayload, {} });
    Require(truncatedPacket.IsOk(), "truncated payload packet should build");
    const auto parsedTruncated = ParseQuicLongHeader(
        truncatedPacket.Value().data(), truncatedPacket.Value().size());
    Require(parsedTruncated.IsOk(), "truncated payload header should parse");
    Require(!DispatchPayload(parsedTruncated.Value().payload, observations)
        && observations.empty(),
        "truncated payload frame must reject without partial observations");

    auto malformedHeader = packet.Value();
    malformedHeader[0] = 0x40;
    Require(!ParseQuicLongHeader(
        malformedHeader.data(), malformedHeader.size()).IsOk(),
        "malformed long-header fixed bit must reject");

    std::cout << "passed=true"
              << " header_roundtrip=true"
              << " header_coordinates=true"
              << " payload_dispatch=true"
              << " payload_coordinates=true"
              << " external_trailing_rejected=true"
              << " unknown_payload_rejected=true"
              << " truncated_payload_rejected=true"
              << " malformed_header_rejected=true"
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
