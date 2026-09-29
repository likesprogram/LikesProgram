#include <LikesProgram/Quic/QuicPacketProtection.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicPacketProtectionError ValidateQuicPacketProtectionRequest(
            const QuicPacketProtectionRequest& request,
            std::span<const std::uint8_t> output) noexcept {
            switch (request.level) {
            case QuicPacketProtectionLevel::Initial:
            case QuicPacketProtectionLevel::Handshake:
            case QuicPacketProtectionLevel::ZeroRtt:
            case QuicPacketProtectionLevel::OneRtt:
                break;
            default:
                return QuicPacketProtectionError::UnsupportedLevel;
            }
            if (request.associatedData.empty()) {
                return QuicPacketProtectionError::InvalidInput;
            }
            if (output.size() < request.payload.size()) {
                return QuicPacketProtectionError::OutputTooSmall;
            }
            return QuicPacketProtectionError::None;
        }

        QuicPacketHeaderProtectionError ValidateQuicShortHeaderProtectionRequest(
            const QuicShortHeaderProtectionRequest& request,
            std::span<const std::uint8_t> output) noexcept {
            if (request.level != QuicPacketProtectionLevel::OneRtt) {
                return QuicPacketHeaderProtectionError::UnsupportedLevel;
            }
            if (request.destinationConnectionIdLength > 20
                || request.packet.size() <= 1 + request.destinationConnectionIdLength) {
                return QuicPacketHeaderProtectionError::InvalidInput;
            }
            if (output.size() < request.packet.size()) {
                return QuicPacketHeaderProtectionError::OutputTooSmall;
            }
            return QuicPacketHeaderProtectionError::None;
        }

        QuicPacketHeaderProtectionError ValidateQuicShortHeaderProtectionResult(
            const QuicShortHeaderProtectionRequest& request,
            const QuicShortHeaderProtectionResult& result,
            std::span<const std::uint8_t> output) noexcept {
            const auto requestValidation = ValidateQuicShortHeaderProtectionRequest(
                request, output);
            if (requestValidation != QuicPacketHeaderProtectionError::None) {
                return requestValidation;
            }
            if (result.error != QuicPacketHeaderProtectionError::None) {
                return result.error;
            }
            if (result.bytesWritten != request.packet.size()
                || result.bytesWritten > output.size()) {
                return QuicPacketHeaderProtectionError::InvalidInput;
            }
            if (result.packetNumberLength < 1 || result.packetNumberLength > 4) {
                return QuicPacketHeaderProtectionError::InvalidPacketNumber;
            }
            return QuicPacketHeaderProtectionError::None;
        }

        QuicPacketHeaderProtectionError ValidateQuicLongHeaderProtectionRequest(
            const QuicLongHeaderProtectionRequest& request,
            std::span<const std::uint8_t> output) noexcept {
            if (request.level != QuicPacketProtectionLevel::Initial
                && request.level != QuicPacketProtectionLevel::Handshake
                && request.level != QuicPacketProtectionLevel::ZeroRtt) {
                return QuicPacketHeaderProtectionError::UnsupportedLevel;
            }
            if (request.packetNumberOffset == 0
                || request.packetNumberOffset >= request.packet.size()
                || request.packetNumberLength > 4
                || request.packet.size() < request.packetNumberOffset + 4 + 16) {
                return QuicPacketHeaderProtectionError::InvalidInput;
            }
            if (output.size() < request.packet.size()) {
                return QuicPacketHeaderProtectionError::OutputTooSmall;
            }
            return QuicPacketHeaderProtectionError::None;
        }

        QuicPacketHeaderProtectionError ValidateQuicLongHeaderProtectionResult(
            const QuicLongHeaderProtectionRequest& request,
            const QuicLongHeaderProtectionResult& result,
            std::span<const std::uint8_t> output) noexcept {
            const auto requestValidation = ValidateQuicLongHeaderProtectionRequest(
                request, output);
            if (requestValidation != QuicPacketHeaderProtectionError::None) {
                return requestValidation;
            }
            if (result.error != QuicPacketHeaderProtectionError::None) {
                return result.error;
            }
            if (result.bytesWritten != request.packet.size()
                || result.bytesWritten > output.size()
                || result.packetNumberLength < 1
                || result.packetNumberLength > 4
                || request.packetNumberOffset + result.packetNumberLength
                    > result.bytesWritten) {
                return QuicPacketHeaderProtectionError::InvalidPacketNumber;
            }
            if (request.packetNumberLength != 0
                && request.packetNumberLength != result.packetNumberLength) {
                return QuicPacketHeaderProtectionError::InvalidPacketNumber;
            }
            return QuicPacketHeaderProtectionError::None;
        }
    }
}
