#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPacketNumber.hpp>
#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>
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
        std::size_t payloadOffset = 0;
        std::size_t consumedBytes = 0;

        bool operator==(const FrameObservation&) const = default;
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

    std::uint64_t ReadPacketNumber(std::span<const std::uint8_t> bytes) {
        std::uint64_t value = 0;
        for (const auto byte : bytes) value = (value << 8) | byte;
        return value;
    }

    bool DispatchPayload(std::span<const std::uint8_t> payload,
        std::vector<FrameObservation>& observations) {
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
            case QuicFrameType::Crypto: {
                const auto parsed = ParseQuicCryptoFrame(
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

    bool ProcessPacket(std::span<const std::uint8_t> packet,
        std::span<const std::uint8_t> destination,
        std::span<const std::uint8_t> source,
        std::span<const std::uint8_t> token,
        QuicPacketNumberSpace& numberSpace,
        std::vector<FrameObservation>& observations,
        std::uint64_t& fullPacketNumber,
        std::size_t& payloadOffset) {
        QuicPacketNumberSpace pendingSpace = numberSpace;
        std::vector<FrameObservation> pendingObservations;
        const auto parsed = ParseQuicLongHeader(packet.data(), packet.size());
        if (!parsed.IsOk()
            || parsed.Value().type != QuicLongPacketType::Initial
            || !SameBytes(parsed.Value().destinationConnectionId, destination)
            || !SameBytes(parsed.Value().sourceConnectionId, source)
            || !SameBytes(parsed.Value().token, token)) {
            return false;
        }

        const auto truncated = ReadPacketNumber(parsed.Value().packetNumber);
        const auto decoded = pendingSpace.Decode(
            truncated, parsed.Value().packetNumberLength);
        if (!decoded.IsOk() || !pendingSpace.Observe(decoded.Value()).IsOk()) {
            return false;
        }
        if (!DispatchPayload(parsed.Value().payload, pendingObservations)) {
            return false;
        }

        numberSpace = pendingSpace;
        observations = std::move(pendingObservations);
        fullPacketNumber = decoded.Value();
        payloadOffset = parsed.Value().consumedBytes
            - parsed.Value().payload.size();
        return true;
    }

    std::vector<std::uint8_t> BuildPacket(
        std::uint64_t fullPacketNumber,
        std::uint64_t largestAcknowledged,
        std::span<const std::uint8_t> destination,
        std::span<const std::uint8_t> source,
        std::span<const std::uint8_t> token,
        std::span<const std::uint8_t> payload) {
        const auto encoding = EncodeQuicPacketNumber(
            fullPacketNumber, largestAcknowledged);
        Require(encoding.IsOk(), "packet number should encode");
        const auto packet = BuildQuicLongHeaderPacket({
            QuicLongPacketType::Initial,
            1,
            destination,
            source,
            token,
            std::span<const std::uint8_t>(encoding.Value().Data(),
                encoding.Value().size),
            payload,
            {} });
        Require(packet.IsOk(), "Initial long header should build");
        return packet.Value();
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 2> cryptoData{ 0xC0, 0xC1 };
    const std::array<std::uint8_t, 2> streamData{ 0xD0, 0xD1 };
    const QuicStreamFrameBuildOptions stream{
        7, 2, true, true, true, streamData };
    std::vector<std::uint8_t> payload;
    Require(Append(payload, BuildQuicPingFrame()), "PING should build");
    Require(Append(payload, BuildQuicCryptoFrame({ 0, cryptoData })),
        "CRYPTO should build");
    Require(Append(payload, BuildQuicStreamFrame(stream)),
        "STREAM should build");

    const std::array<std::uint8_t, 4> destination{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 3> source{ 5, 6, 7 };
    const std::array<std::uint8_t, 2> token{ 0xE0, 0xE1 };
    constexpr std::uint64_t largestAcknowledged = 0xABE8B3;
    constexpr std::uint64_t firstPacketNumber = 0xAC5C02;
    constexpr std::uint64_t olderPacketNumber = 0xAC5C01;
    const auto firstPacket = BuildPacket(firstPacketNumber,
        largestAcknowledged, destination, source, token, payload);
    const auto olderPacket = BuildPacket(olderPacketNumber,
        largestAcknowledged, destination, source, token, payload);

    QuicPacketNumberSpace numberSpace;
    Require(numberSpace.Observe(largestAcknowledged).IsOk(),
        "number-space baseline should observe");
    std::vector<FrameObservation> observations;
    std::uint64_t decoded = 0;
    std::size_t payloadOffset = 0;
    Require(ProcessPacket(firstPacket, destination, source, token,
        numberSpace, observations, decoded, payloadOffset)
        && decoded == firstPacketNumber
        && observations.size() == 3
        && numberSpace.Snapshot().largestReceived == firstPacketNumber,
        "Initial packet number should reconstruct before dispatch");
    const auto firstHeader = ParseQuicLongHeader(
        firstPacket.data(), firstPacket.size());
    Require(firstHeader.IsOk()
        && firstHeader.Value().packetNumberLength > 0
        && firstHeader.Value().token.size() == token.size()
        && payloadOffset > destination.size() + source.size(),
        "Initial header coordinates should be exposed");
    Require(observations[0].kind == QuicFrameType::Ping
        && observations[1].kind == QuicFrameType::Crypto
        && observations[2].kind == QuicFrameType::Stream,
        "Initial payload frame sequence should dispatch");

    const auto baselineSnapshot = numberSpace.Snapshot();
    const auto baselineObservations = observations;
    Require(ProcessPacket(olderPacket, destination, source, token,
        numberSpace, observations, decoded, payloadOffset)
        && decoded == olderPacketNumber
        && numberSpace.Snapshot().largestReceived
            == baselineSnapshot.largestReceived,
        "older Initial packet must not regress number-space state");
    Require(ProcessPacket(firstPacket, destination, source, token,
        numberSpace, observations, decoded, payloadOffset)
        && decoded == firstPacketNumber
        && numberSpace.Snapshot().largestReceived
            == baselineSnapshot.largestReceived,
        "duplicate Initial packet should be deterministic");

    auto truncatedPayload = firstPacket;
    truncatedPayload.pop_back();
    auto rejectedSpace = numberSpace;
    auto rejectedObservations = baselineObservations;
    const auto rejectedSnapshot = rejectedSpace.Snapshot();
    Require(!ProcessPacket(truncatedPayload, destination, source, token,
        rejectedSpace, rejectedObservations, decoded, payloadOffset)
        && rejectedSpace.Snapshot().largestReceived
            == rejectedSnapshot.largestReceived
        && rejectedObservations == baselineObservations,
        "truncated Initial payload must reject atomically");

    auto malformedHeader = firstPacket;
    malformedHeader[0] = 0x00;
    rejectedSpace = numberSpace;
    rejectedObservations = baselineObservations;
    Require(!ProcessPacket(malformedHeader, destination, source, token,
        rejectedSpace, rejectedObservations, decoded, payloadOffset)
        && rejectedSpace.Snapshot().largestReceived
            == rejectedSnapshot.largestReceived
        && rejectedObservations == baselineObservations,
        "malformed Initial header must reject atomically");

    const auto packetNumberOffset = payloadOffset
        - firstHeader.Value().packetNumberLength;
    auto truncatedPacketNumber = firstPacket;
    truncatedPacketNumber.resize(packetNumberOffset
        + firstHeader.Value().packetNumberLength - 1);
    rejectedSpace = numberSpace;
    rejectedObservations = baselineObservations;
    Require(!ProcessPacket(truncatedPacketNumber, destination, source, token,
        rejectedSpace, rejectedObservations, decoded, payloadOffset)
        && rejectedSpace.Snapshot().largestReceived
            == rejectedSnapshot.largestReceived
        && rejectedObservations == baselineObservations,
        "truncated Initial packet number must reject atomically");

    std::cout << "passed=true"
              << " header_roundtrip=true"
              << " token_coordinates=true"
              << " packet_number_reconstructed=true"
              << " payload_dispatch=true"
              << " older_nonregression=true"
              << " duplicate_deterministic=true"
              << " truncated_payload_rejected=true"
              << " malformed_header_rejected=true"
              << " truncated_packet_number_rejected=true"
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
