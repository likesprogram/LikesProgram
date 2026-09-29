#include <LikesProgram/Quic/QuicLongHeader.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool SameBytes(std::span<const std::uint8_t> left,
        std::span<const std::uint8_t> right) {
        return left.size() == right.size()
            && std::equal(left.begin(), left.end(), right.begin());
    }

    void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value >> 24));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 4> destination{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 3> source{ 5, 6, 7 };
    const std::array<std::uint8_t, 5> token{ 0xA0, 0xA1, 0xA2, 0xA3, 0xA4 };
    const std::array<std::uint8_t, 16> integrityTag{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

    const auto retry = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Retry, 1, destination, source, token, {}, {},
        integrityTag });
    Require(retry.IsOk(), "Retry packet should compose from caller spans");
    Require(retry.Value().front() == 0xF0,
        "Retry packet type must remain in the long-header type bits");
    const auto retryView = ParseQuicLongHeader(
        retry.Value().data(), retry.Value().size());
    Require(retryView.IsOk()
        && retryView.Value().type == QuicLongPacketType::Retry
        && retryView.Value().version == 1
        && SameBytes(retryView.Value().destinationConnectionId, destination)
        && SameBytes(retryView.Value().sourceConnectionId, source)
        && SameBytes(retryView.Value().token, token)
        && SameBytes(retryView.Value().retryIntegrityTag, integrityTag)
        && retryView.Value().packetNumber.empty()
        && retryView.Value().payload.empty()
        && retryView.Value().consumedBytes == retry.Value().size(),
        "Retry parse must preserve coordinates and consume the complete datagram");

    QuicLongHeaderBuildOptions invalidRetry{
        QuicLongPacketType::Retry, 1, destination, source, {}, {}, {},
        integrityTag };
    Require(!BuildQuicLongHeaderPacket(invalidRetry).IsOk(),
        "empty Retry token must be rejected");
    invalidRetry.token = token;
    const std::array<std::uint8_t, 15> shortTag{};
    invalidRetry.retryIntegrityTag = shortTag;
    Require(!BuildQuicLongHeaderPacket(invalidRetry).IsOk(),
        "short Retry integrity tag must be rejected");
    invalidRetry.retryIntegrityTag = integrityTag;
    const std::array<std::uint8_t, 1> packetNumber{ 1 };
    invalidRetry.packetNumber = packetNumber;
    Require(!BuildQuicLongHeaderPacket(invalidRetry).IsOk(),
        "Retry packet number must be rejected");

    auto truncatedRetry = retry.Value();
    truncatedRetry.resize(truncatedRetry.size() - token.size() - 1);
    Require(!ParseQuicLongHeader(
        truncatedRetry.data(), truncatedRetry.size()).IsOk(),
        "truncated Retry integrity tag must be rejected");

    std::vector<std::uint8_t> versionNegotiation{ 0x80, 0, 0, 0, 0,
        static_cast<std::uint8_t>(destination.size()) };
    versionNegotiation.insert(versionNegotiation.end(), destination.begin(),
        destination.end());
    versionNegotiation.push_back(static_cast<std::uint8_t>(source.size()));
    versionNegotiation.insert(versionNegotiation.end(), source.begin(),
        source.end());
    AppendU32(versionNegotiation, 1);
    AppendU32(versionNegotiation, 2);
    const auto versionView = ParseQuicLongHeader(
        versionNegotiation.data(), versionNegotiation.size());
    Require(versionView.IsOk()
        && versionView.Value().type == QuicLongPacketType::VersionNegotiation
        && versionView.Value().version == 0
        && SameBytes(versionView.Value().destinationConnectionId, destination)
        && SameBytes(versionView.Value().sourceConnectionId, source)
        && versionView.Value().versionNegotiationVersions.size() == 8
        && versionView.Value().versionNegotiationVersions.size() % 4 == 0
        && versionView.Value().consumedBytes == versionNegotiation.size(),
        "Version Negotiation parse must preserve ids and aligned versions");

    auto misalignedVersions = versionNegotiation;
    misalignedVersions.pop_back();
    Require(!ParseQuicLongHeader(
        misalignedVersions.data(), misalignedVersions.size()).IsOk(),
        "misaligned Version Negotiation versions must be rejected");
    auto emptyVersions = versionNegotiation;
    emptyVersions.resize(emptyVersions.size() - 8);
    Require(!ParseQuicLongHeader(
        emptyVersions.data(), emptyVersions.size()).IsOk(),
        "empty Version Negotiation versions must be rejected");
    const std::array<std::uint8_t, 1> tooShort{ 0x80 };
    Require(!ParseQuicLongHeader(tooShort.data(), tooShort.size()).IsOk(),
        "truncated long header must be rejected");

    const std::array<std::uint8_t, 1> packetPayload{ 0xEE };
    const auto initial = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, {}, packetNumber,
        packetPayload, {} });
    Require(initial.IsOk(), "Initial packet should compose for boundary check");
    auto initialWithTrailing = initial.Value();
    initialWithTrailing.push_back(0xA5);
    const auto initialView = ParseQuicLongHeader(
        initialWithTrailing.data(), initialWithTrailing.size());
    Require(initialView.IsOk()
        && initialView.Value().consumedBytes + 1 == initialWithTrailing.size(),
        "payload packet parser must preserve a trailing datagram boundary");

    std::cout << "passed=true"
              << " retry_roundtrip=true"
              << " retry_integrity_span=true"
              << " retry_coordinates=true"
              << " retry_exact_consumed=true"
              << " version_negotiation=true"
              << " versions_aligned=true"
              << " rejection_preserves=true"
              << " initial_boundary=true\n";
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
