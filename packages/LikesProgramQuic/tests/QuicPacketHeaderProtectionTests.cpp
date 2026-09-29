#include <LikesProgram/Quic/QuicPacketProtection.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    class MarkerProvider final
        : public LikesProgram::Quic::QuicPacketHeaderProtectionProvider {
    public:
        LikesProgram::Quic::QuicShortHeaderProtectionResult ProtectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

        LikesProgram::Quic::QuicShortHeaderProtectionResult UnprotectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

    private:
        static LikesProgram::Quic::QuicShortHeaderProtectionResult Transform(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation =
                LikesProgram::Quic::ValidateQuicShortHeaderProtectionRequest(
                    request,
                    std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != LikesProgram::Quic::QuicPacketHeaderProtectionError::None) {
                return { validation, 0, 0, 0 };
            }
            std::copy(request.packet.begin(), request.packet.end(), output.begin());
            return { LikesProgram::Quic::QuicPacketHeaderProtectionError::None,
                request.packet.size(), 0x1234, 2 };
        }
    };

    class LongMarkerProvider final
        : public LikesProgram::Quic::QuicPacketHeaderProtectionProvider {
    public:
        LikesProgram::Quic::QuicShortHeaderProtectionResult ProtectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest&,
            std::span<std::uint8_t>) noexcept override {
            return { LikesProgram::Quic::QuicPacketHeaderProtectionError::NotReady,
                0, 0, 0 };
        }

        LikesProgram::Quic::QuicShortHeaderProtectionResult UnprotectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest&,
            std::span<std::uint8_t>) noexcept override {
            return { LikesProgram::Quic::QuicPacketHeaderProtectionError::NotReady,
                0, 0, 0 };
        }

        LikesProgram::Quic::QuicLongHeaderProtectionResult ProtectLongHeader(
            const LikesProgram::Quic::QuicLongHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

        LikesProgram::Quic::QuicLongHeaderProtectionResult UnprotectLongHeader(
            const LikesProgram::Quic::QuicLongHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

    private:
        static LikesProgram::Quic::QuicLongHeaderProtectionResult Transform(
            const LikesProgram::Quic::QuicLongHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation =
                LikesProgram::Quic::ValidateQuicLongHeaderProtectionRequest(
                    request,
                    std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != LikesProgram::Quic::QuicPacketHeaderProtectionError::None) {
                return { validation, 0, 0, 0 };
            }
            std::copy(request.packet.begin(), request.packet.end(), output.begin());
            return { LikesProgram::Quic::QuicPacketHeaderProtectionError::None,
                request.packet.size(), 0x1234, 2 };
        }
    };
}

void RunQuicPacketHeaderProtectionTests() {
    using namespace LikesProgram::Quic;

    const std::array<std::uint8_t, 4> packet{ 0x41, 0x01, 0x02, 0x03 };
    const QuicShortHeaderProtectionRequest request{
        QuicPacketProtectionLevel::OneRtt, 0x1200, 1, packet };
    std::array<std::uint8_t, 4> output{};
    Require(ValidateQuicShortHeaderProtectionRequest(request, output)
        == QuicPacketHeaderProtectionError::None);

    MarkerProvider provider;
    const auto unprotected = provider.UnprotectShortHeader(request, output);
    Require(unprotected.Succeeded());
    Require(ValidateQuicShortHeaderProtectionResult(
                request, unprotected, output)
        == QuicPacketHeaderProtectionError::None);
    Require(std::equal(packet.begin(), packet.end(), output.begin()));
    Require(unprotected.packetNumber == 0x1234);
    Require(unprotected.packetNumberLength == 2);

    std::array<std::uint8_t, 4> protectedOutput{};
    const auto protectedPacket = provider.ProtectShortHeader(request, protectedOutput);
    Require(protectedPacket.Succeeded());
    Require(std::equal(packet.begin(), packet.end(), protectedOutput.begin()));

    const auto tooSmall = ValidateQuicShortHeaderProtectionRequest(
        request, std::span<const std::uint8_t>(output.data(), output.size() - 1));
    Require(tooSmall == QuicPacketHeaderProtectionError::OutputTooSmall);

    auto invalidLevel = request;
    invalidLevel.level = QuicPacketProtectionLevel::Handshake;
    Require(ValidateQuicShortHeaderProtectionRequest(invalidLevel, output)
        == QuicPacketHeaderProtectionError::UnsupportedLevel);

    auto invalidCid = request;
    invalidCid.destinationConnectionIdLength = 21;
    Require(ValidateQuicShortHeaderProtectionRequest(invalidCid, output)
        == QuicPacketHeaderProtectionError::InvalidInput);

    auto invalidPacket = request;
    invalidPacket.packet = std::span<const std::uint8_t>(packet.data(), 2);
    Require(ValidateQuicShortHeaderProtectionRequest(invalidPacket, output)
        == QuicPacketHeaderProtectionError::InvalidInput);

    auto invalidResult = unprotected;
    invalidResult.packetNumberLength = 0;
    Require(ValidateQuicShortHeaderProtectionResult(
                request, invalidResult, output)
        == QuicPacketHeaderProtectionError::InvalidPacketNumber);

    std::array<std::uint8_t, 40> longPacket{};
    longPacket[0] = 0xC1;
    const QuicLongHeaderProtectionRequest longRequest{
        QuicPacketProtectionLevel::Initial, 0x1200, 8, 2, longPacket };
    std::array<std::uint8_t, 40> longOutput{};
    Require(ValidateQuicLongHeaderProtectionRequest(longRequest, longOutput)
        == QuicPacketHeaderProtectionError::None);
    LongMarkerProvider longProvider;
    const auto longProtected = longProvider.ProtectLongHeader(
        longRequest, longOutput);
    Require(longProtected.Succeeded());
    Require(ValidateQuicLongHeaderProtectionResult(
                longRequest, longProtected, longOutput)
        == QuicPacketHeaderProtectionError::None);
    Require(longProtected.packetNumber == 0x1234);
    Require(longProtected.packetNumberLength == 2);
    const auto longUnprotected = longProvider.UnprotectLongHeader(
        longRequest, longOutput);
    Require(longUnprotected.Succeeded());
    auto invalidLong = longRequest;
    invalidLong.level = QuicPacketProtectionLevel::OneRtt;
    Require(ValidateQuicLongHeaderProtectionRequest(invalidLong, longOutput)
        == QuicPacketHeaderProtectionError::UnsupportedLevel);
}
