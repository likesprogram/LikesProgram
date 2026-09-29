#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicFrameType.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>

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

        friend bool operator==(const FrameObservation&,
            const FrameObservation&) = default;
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

    void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
        output.push_back(static_cast<std::uint8_t>(value >> 24));
        output.push_back(static_cast<std::uint8_t>(value >> 16));
        output.push_back(static_cast<std::uint8_t>(value >> 8));
        output.push_back(static_cast<std::uint8_t>(value));
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const QuicAckFrame ack{
        false, 31, 1, { { 30, 31 } }, 0, 0, 0, 0 };
    std::vector<std::uint8_t> payload;
    Require(Append(payload, BuildQuicPingFrame()), "PING should build");
    Require(Append(payload, BuildQuicAckFrame(ack)), "ACK should build");

    const std::array<std::uint8_t, 4> destination{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 3> source{ 5, 6, 7 };
    const std::array<std::uint8_t, 1> token{ 0xB0 };
    const std::array<std::uint8_t, 1> initialPacketNumber{ 1 };
    const std::array<std::uint8_t, 1> zeroRttPacketNumber{ 2 };
    const std::array<std::uint8_t, 1> handshakePacketNumber{ 3 };

    const auto initial = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, token,
        initialPacketNumber, payload, {} });
    const auto zeroRtt = BuildQuicLongHeaderPacket({
        QuicLongPacketType::ZeroRtt, 1, destination, source, {},
        zeroRttPacketNumber, payload, {} });
    const auto handshake = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Handshake, 1, destination, source, {},
        handshakePacketNumber, payload, {} });
    Require(initial.IsOk() && zeroRtt.IsOk() && handshake.IsOk(),
        "Initial, 0-RTT and Handshake packets should build");

    const auto parsedInitial = ParseQuicLongHeader(
        initial.Value().data(), initial.Value().size());
    const auto parsedZeroRtt = ParseQuicLongHeader(
        zeroRtt.Value().data(), zeroRtt.Value().size());
    const auto parsedHandshake = ParseQuicLongHeader(
        handshake.Value().data(), handshake.Value().size());
    Require(parsedInitial.IsOk() && parsedZeroRtt.IsOk()
        && parsedHandshake.IsOk()
        && parsedInitial.Value().type == QuicLongPacketType::Initial
        && parsedZeroRtt.Value().type == QuicLongPacketType::ZeroRtt
        && parsedHandshake.Value().type == QuicLongPacketType::Handshake
        && SameBytes(parsedInitial.Value().token, token)
        && parsedZeroRtt.Value().token.empty()
        && parsedHandshake.Value().token.empty()
        && SameBytes(parsedInitial.Value().payload, payload)
        && SameBytes(parsedZeroRtt.Value().payload, payload)
        && SameBytes(parsedHandshake.Value().payload, payload),
        "long-header type matrix must preserve token and payload rules");

    std::vector<FrameObservation> observations;
    Require(DispatchPayload(parsedInitial.Value().payload, observations)
        && observations.size() == 2,
        "Initial payload should dispatch");
    const auto initialObservations = observations;
    Require(DispatchPayload(parsedZeroRtt.Value().payload, observations)
        && observations == initialObservations,
        "0-RTT payload should dispatch with the same coordinates");
    Require(DispatchPayload(parsedHandshake.Value().payload, observations)
        && observations == initialObservations,
        "Handshake payload should dispatch with the same coordinates");

    const std::array<std::uint8_t, 16> retryTag{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const auto retry = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Retry, 1, destination, source, token, {}, {},
        retryTag });
    Require(retry.IsOk(), "Retry packet without payload should build");
    const auto parsedRetry = ParseQuicLongHeader(
        retry.Value().data(), retry.Value().size());
    Require(parsedRetry.IsOk() && parsedRetry.Value().type == QuicLongPacketType::Retry
        && parsedRetry.Value().payload.empty()
        && parsedRetry.Value().retryIntegrityTag.size() == retryTag.size(),
        "Retry must remain a token/tag packet without payload frames");

    QuicLongHeaderBuildOptions invalidRetry{
        QuicLongPacketType::Retry, 1, destination, source, token, {}, payload,
        retryTag };
    Require(!BuildQuicLongHeaderPacket(invalidRetry).IsOk(),
        "Retry payload must be rejected");
    invalidRetry.payload = {};
    invalidRetry.packetNumber = initialPacketNumber;
    Require(!BuildQuicLongHeaderPacket(invalidRetry).IsOk(),
        "Retry packet number must be rejected");

    std::vector<std::uint8_t> versionNegotiation{ 0x80, 0, 0, 0, 0,
        static_cast<std::uint8_t>(destination.size()) };
    versionNegotiation.insert(versionNegotiation.end(), destination.begin(), destination.end());
    versionNegotiation.push_back(static_cast<std::uint8_t>(source.size()));
    versionNegotiation.insert(versionNegotiation.end(), source.begin(), source.end());
    AppendU32(versionNegotiation, 1);
    AppendU32(versionNegotiation, 0xfaceb00c);
    const auto parsedVersionNegotiation = ParseQuicLongHeader(
        versionNegotiation.data(), versionNegotiation.size());
    Require(parsedVersionNegotiation.IsOk()
        && parsedVersionNegotiation.Value().type == QuicLongPacketType::VersionNegotiation
        && parsedVersionNegotiation.Value().versionNegotiationVersions.size() == 8
        && parsedVersionNegotiation.Value().consumedBytes == versionNegotiation.size(),
        "Version Negotiation must remain parse-only and 4-byte aligned");

    auto unknownPayload = payload;
    unknownPayload.push_back(0x2A);
    const auto unknownPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, token,
        initialPacketNumber, unknownPayload, {} });
    Require(unknownPacket.IsOk(), "unknown payload packet should build");
    const auto parsedUnknown = ParseQuicLongHeader(
        unknownPacket.Value().data(), unknownPacket.Value().size());
    Require(parsedUnknown.IsOk()
        && !DispatchPayload(parsedUnknown.Value().payload, observations)
        && observations.empty(),
        "unknown payload frame must reject atomically");

    std::cout << "passed=true"
              << " type_matrix=true"
              << " token_rules=true"
              << " payload_dispatch=true"
              << " retry_no_payload=true"
              << " retry_rejected=true"
              << " version_negotiation=true"
              << " unknown_rejected=true"
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
