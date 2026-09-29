#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicShortHeader.hpp>
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
    const std::array<std::uint8_t, 2> streamData{ 0xE0, 0xE1 };
    const QuicStreamFrameBuildOptions stream{
        8, 3, true, true, false, streamData };
    const QuicAckFrame ack{
        false, 21, 2, { { 20, 21 } }, 0, 0, 0, 0 };

    std::vector<std::uint8_t> payload;
    Require(Append(payload, BuildQuicPingFrame()), "PING should build");
    Require(Append(payload, BuildQuicAckFrame(ack)), "ACK should build");
    Require(Append(payload, BuildQuicStreamFrame(stream)),
        "STREAM should build");

    const std::array<std::uint8_t, 5> destination{ 1, 2, 3, 4, 5 };
    const std::array<std::uint8_t, 2> packetNumber{ 0x03, 0x04 };
    const auto packet = BuildQuicShortHeaderPacket({
        true, true, destination, packetNumber, payload });
    Require(packet.IsOk(), "short-header packet should build around payload bytes");

    const auto parsed = ParseQuicShortHeader(
        packet.Value().data(), packet.Value().size(), destination.size());
    Require(parsed.IsOk()
        && parsed.Value().spinBit && parsed.Value().keyPhase
        && parsed.Value().packetNumberLength == packetNumber.size()
        && SameBytes(parsed.Value().destinationConnectionId, destination)
        && SameBytes(parsed.Value().packetNumber, packetNumber)
        && SameBytes(parsed.Value().payload, payload)
        && parsed.Value().consumedBytes == packet.Value().size(),
        "short header must preserve packet and payload coordinates");

    std::vector<FrameObservation> observations;
    Require(DispatchPayload(parsed.Value().payload, observations)
        && observations.size() == 3,
        "parsed short payload sequence should dispatch completely");
    const std::array<QuicFrameType, 3> expectedKinds{
        QuicFrameType::Ping, QuicFrameType::Ack, QuicFrameType::Stream };
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        Require(observations[index].kind == expectedKinds[index]
            && observations[index].offset == expectedOffset
            && observations[index].consumedBytes > 0,
            "short payload frame coordinates should be relative to payload");
        expectedOffset += observations[index].consumedBytes;
    }
    Require(expectedOffset == parsed.Value().payload.size(),
        "short payload dispatch must consume the exact payload span");

    auto unknownPayload = payload;
    unknownPayload.push_back(0x2A);
    const auto unknownPacket = BuildQuicShortHeaderPacket({
        true, true, destination, packetNumber, unknownPayload });
    Require(unknownPacket.IsOk(), "unknown short payload packet should build");
    const auto parsedUnknown = ParseQuicShortHeader(
        unknownPacket.Value().data(), unknownPacket.Value().size(), destination.size());
    Require(parsedUnknown.IsOk(), "unknown short payload header should parse");
    Require(!DispatchPayload(parsedUnknown.Value().payload, observations)
        && observations.empty(),
        "unknown short payload frame must reject atomically");

    auto truncatedPayload = payload;
    truncatedPayload.pop_back();
    const auto truncatedPacket = BuildQuicShortHeaderPacket({
        true, true, destination, packetNumber, truncatedPayload });
    Require(truncatedPacket.IsOk(), "truncated short payload packet should build");
    const auto parsedTruncated = ParseQuicShortHeader(
        truncatedPacket.Value().data(), truncatedPacket.Value().size(), destination.size());
    Require(parsedTruncated.IsOk(), "truncated short payload header should parse");
    Require(!DispatchPayload(parsedTruncated.Value().payload, observations)
        && observations.empty(),
        "truncated short payload frame must reject atomically");

    auto malformedHeader = packet.Value();
    malformedHeader[0] = static_cast<std::uint8_t>(malformedHeader[0] | 0x80);
    Require(!ParseQuicShortHeader(
        malformedHeader.data(), malformedHeader.size(), destination.size()).IsOk(),
        "short header long-bit mutation must reject");
    malformedHeader = packet.Value();
    malformedHeader[0] = static_cast<std::uint8_t>(malformedHeader[0] | 0x18);
    Require(!ParseQuicShortHeader(
        malformedHeader.data(), malformedHeader.size(), destination.size()).IsOk(),
        "short header reserved-bit mutation must reject");

    std::cout << "passed=true"
              << " header_roundtrip=true"
              << " header_coordinates=true"
              << " payload_dispatch=true"
              << " payload_coordinates=true"
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
