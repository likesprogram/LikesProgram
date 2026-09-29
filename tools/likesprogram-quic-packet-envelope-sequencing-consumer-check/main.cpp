#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicPacketNumber.hpp>
#include <LikesProgram/Quic/QuicPacketProtection.hpp>
#include <LikesProgram/Quic/QuicShortHeader.hpp>

#include <array>
#include <algorithm>
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

    class MarkerProvider final : public QuicPacketProtectionProvider {
    public:
        QuicPacketProtectionResult Protect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            ++protectCalls;
            const auto error = ValidateQuicPacketProtectionRequest(request, output);
            if (error != QuicPacketProtectionError::None) return { error, 0 };
            for (std::size_t index = 0; index < request.payload.size(); ++index) {
                output[index] = static_cast<std::uint8_t>(request.payload[index] ^ 0xa5);
            }
            return { QuicPacketProtectionError::None, request.payload.size() };
        }

        QuicPacketProtectionResult Unprotect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            ++unprotectCalls;
            return Protect(request, output);
        }

        int protectCalls = 0;
        int unprotectCalls = 0;
    };
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 4> destination{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 2> source{ 5, 6 };
    const std::vector<std::uint8_t> plaintext{ 0x10, 0x20, 0x30 };
    const auto packetNumber = EncodeQuicPacketNumber(0x102);
    Require(packetNumber.IsOk() && packetNumber.Value().size > 0,
        "caller should derive a bounded packet-number encoding");

    const auto longPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, {},
        std::span<const std::uint8_t>(packetNumber.Value().Data(), packetNumber.Value().size),
        plaintext, {} });
    Require(longPacket.IsOk(),
        "Initial long header should compose opaque packet-number and payload spans");
    const auto longView = ParseQuicLongHeader(
        longPacket.Value().data(), longPacket.Value().size());
    Require(longView.IsOk() && longView.Value().type == QuicLongPacketType::Initial
        && longView.Value().destinationConnectionId.size() == destination.size()
        && longView.Value().sourceConnectionId.size() == source.size()
        && longView.Value().packetNumber.size() == packetNumber.Value().size
        && longView.Value().payload.size() == plaintext.size(),
        "long header should parse its caller-owned coordinates");

    const std::size_t associatedDataBytes = longPacket.Value().size()
        - longView.Value().payload.size();
    MarkerProvider provider;
    std::vector<std::uint8_t> protectedPayload(plaintext.size(), 0);
    const auto protection = provider.Protect({
        QuicPacketProtectionLevel::Initial, 0x102,
        std::span<const std::uint8_t>(longPacket.Value().data(), associatedDataBytes),
        plaintext }, protectedPayload);
    Require(protection.Succeeded() && protection.bytesWritten == plaintext.size()
        && protectedPayload[0] == static_cast<std::uint8_t>(plaintext[0] ^ 0xa5),
        "caller-owned protection should receive explicit associated-data and payload spans");

    const auto protectedLongPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial, 1, destination, source, {},
        std::span<const std::uint8_t>(packetNumber.Value().Data(), packetNumber.Value().size),
        protectedPayload, {} });
    Require(protectedLongPacket.IsOk(),
        "protected payload should reuse the same long-header envelope");
    const auto protectedLongView = ParseQuicLongHeader(
        protectedLongPacket.Value().data(), protectedLongPacket.Value().size());
    Require(protectedLongView.IsOk()
        && SameBytes(protectedLongView.Value().packetNumber,
            longView.Value().packetNumber)
        && protectedLongView.Value().payload.size() == protectedPayload.size(),
        "packet protection should not alter packet-number/header coordinates");
    std::vector<std::uint8_t> roundTrip(plaintext.size(), 0);
    const auto unprotected = provider.Unprotect({
        QuicPacketProtectionLevel::Initial, 0x102,
        std::span<const std::uint8_t>(protectedLongPacket.Value().data(), associatedDataBytes),
        protectedPayload }, roundTrip);
    Require(unprotected.Succeeded() && roundTrip == plaintext,
        "caller-owned unprotect should restore the payload without engine state");

    const auto shortPacket = BuildQuicShortHeaderPacket({
        true, true, destination,
        std::span<const std::uint8_t>(packetNumber.Value().Data(), packetNumber.Value().size),
        protectedPayload });
    Require(shortPacket.IsOk(),
        "short header should compose the same opaque packet-number and payload");
    const auto shortView = ParseQuicShortHeader(
        shortPacket.Value().data(), shortPacket.Value().size(), destination.size());
    Require(shortView.IsOk() && shortView.Value().spinBit && shortView.Value().keyPhase
        && SameBytes(shortView.Value().packetNumber,
            longView.Value().packetNumber)
        && shortView.Value().payload.size() == protectedPayload.size(),
        "short header should preserve flags and packet coordinates");

    const std::uint64_t externalActionId = 41;
    Require(externalActionId != 0 && provider.protectCalls == 2
        && provider.unprotectCalls == 1,
        "action id remains external to the wire and protection provider");
    std::cout << "passed=true"
              << " packet_number_bytes=" << packetNumber.Value().size
              << " long_header=true"
              << " short_header=true"
              << " spans_forwarded=true"
              << " roundtrip=true"
              << " action_id_external=true"
              << " marker_not_crypto=true\n";
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
