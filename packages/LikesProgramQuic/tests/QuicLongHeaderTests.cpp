#include <LikesProgram/Quic/QuicLongHeader.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    std::vector<std::uint8_t> CopySpan(
        std::span<const std::uint8_t> bytes) {
        return { bytes.begin(), bytes.end() };
    }
}

void RunQuicLongHeaderTests() {
    using namespace LikesProgram::Quic;

    const std::array<std::uint8_t, 2> destination{ 0x01, 0x02 };
    const std::array<std::uint8_t, 3> source{ 0x03, 0x04, 0x05 };
    const std::array<std::uint8_t, 1> token{ 0xAA };
    const std::array<std::uint8_t, 2> packetNumber{ 0x12, 0x34 };
    const std::array<std::uint8_t, 3> payload{ 0xC0, 0x01, 0x02 };

    QuicLongHeaderBuildOptions initial;
    initial.type = QuicLongPacketType::Initial;
    initial.destinationConnectionId = destination;
    initial.sourceConnectionId = source;
    initial.token = token;
    initial.packetNumber = packetNumber;
    initial.payload = payload;
    const auto encoded = BuildQuicLongHeaderPacket(initial);
    Require(encoded.IsOk());
    Require(encoded.Value().front() == 0xC1);
    auto parsed = ParseQuicLongHeader(
        encoded.Value().data(), encoded.Value().size());
    Require(parsed.IsOk());
    Require(parsed.Value().type == QuicLongPacketType::Initial);
    Require(parsed.Value().version == 1);
    Require(parsed.Value().destinationConnectionId.size() == 2);
    Require(parsed.Value().sourceConnectionId.size() == 3);
    Require(CopySpan(parsed.Value().token) == std::vector<std::uint8_t>{ 0xAA });
    Require(CopySpan(parsed.Value().packetNumber)
        == std::vector<std::uint8_t>{ 0x12, 0x34 });
    Require(CopySpan(parsed.Value().payload)
        == std::vector<std::uint8_t>{ 0xC0, 0x01, 0x02 });
    Require(parsed.Value().packetNumberLength == 2);
    Require(parsed.Value().packetNumberOffset
        == static_cast<std::size_t>(
            parsed.Value().packetNumber.data() - encoded.Value().data()));
    Require(parsed.Value().consumedBytes == encoded.Value().size());

    auto withTrailing = encoded.Value();
    withTrailing.push_back(0xEE);
    parsed = ParseQuicLongHeader(withTrailing.data(), withTrailing.size());
    Require(parsed.IsOk());
    Require(parsed.Value().consumedBytes + 1 == withTrailing.size());

    QuicLongHeaderBuildOptions retry;
    retry.type = QuicLongPacketType::Retry;
    retry.destinationConnectionId = destination;
    retry.sourceConnectionId = source;
    const std::array<std::uint8_t, 2> retryToken{ 0x10, 0x11 };
    const std::array<std::uint8_t, 16> retryTag{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    retry.token = retryToken;
    retry.retryIntegrityTag = retryTag;
    const auto retryEncoded = BuildQuicLongHeaderPacket(retry);
    Require(retryEncoded.IsOk());
    parsed = ParseQuicLongHeader(
        retryEncoded.Value().data(), retryEncoded.Value().size());
    Require(parsed.IsOk());
    Require(parsed.Value().type == QuicLongPacketType::Retry);
    Require(parsed.Value().packetNumber.empty());
    Require(parsed.Value().payload.empty());
    Require(parsed.Value().retryIntegrityTag.size() == 16);

    const std::array<std::uint8_t, 20> versionNegotiation{
        0x80, 0, 0, 0, 0, 2, 1, 2, 3, 3,
        4, 5, 6, 0, 0, 0, 1, 0, 0, 0 };
    parsed = ParseQuicLongHeader(
        versionNegotiation.data(), versionNegotiation.size());
    Require(parsed.IsOk());
    Require(parsed.Value().type == QuicLongPacketType::VersionNegotiation);
    Require(parsed.Value().version == 0);
    Require(parsed.Value().versionNegotiationVersions.size() == 8);
    Require(parsed.Value().consumedBytes == versionNegotiation.size());

    auto invalid = initial;
    invalid.type = QuicLongPacketType::ZeroRtt;
    invalid.token = token;
    Require(!BuildQuicLongHeaderPacket(invalid).IsOk());
    invalid = initial;
    invalid.packetNumber = {};
    Require(!BuildQuicLongHeaderPacket(invalid).IsOk());
    invalid = initial;
    const std::array<std::uint8_t, 5> tooLongPacketNumber{ 0, 1, 2, 3, 4 };
    invalid.packetNumber = tooLongPacketNumber;
    Require(!BuildQuicLongHeaderPacket(invalid).IsOk());
    invalid = initial;
    const std::array<std::uint8_t, 21> tooLongConnectionId{};
    invalid.destinationConnectionId = tooLongConnectionId;
    Require(!BuildQuicLongHeaderPacket(invalid).IsOk());

    const std::array<std::uint8_t, 1> shortPacket{ 0xC0 };
    Require(!ParseQuicLongHeader(shortPacket.data(), shortPacket.size()).IsOk());
    auto malformed = encoded.Value();
    malformed[0] = 0x80;
    Require(!ParseQuicLongHeader(malformed.data(), malformed.size()).IsOk());
    malformed = retryEncoded.Value();
    malformed.resize(malformed.size() - 3);
    Require(!ParseQuicLongHeader(malformed.data(), malformed.size()).IsOk());
    malformed = encoded.Value();
    malformed.resize(malformed.size() - 1);
    Require(!ParseQuicLongHeader(malformed.data(), malformed.size()).IsOk());
    const std::array<std::uint8_t, 18> badVersions{
        0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1 };
    Require(!ParseQuicLongHeader(badVersions.data(), badVersions.size()).IsOk());
}
