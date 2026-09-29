#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace LikesProgram {
    namespace Quic {
        enum class QuicPacketProtectionLevel : std::uint8_t {
            Initial,
            Handshake,
            ZeroRtt,
            OneRtt
        };

        enum class QuicPacketProtectionError : int {
            None = 0,
            InvalidInput = 1,
            OutputTooSmall = 2,
            NotReady = 3,
            AuthenticationFailed = 4,
            UnsupportedLevel = 5
        };

        struct QuicPacketProtectionRequest {
            QuicPacketProtectionLevel level = QuicPacketProtectionLevel::Initial;
            std::uint64_t packetNumber = 0;
            std::span<const std::uint8_t> associatedData{};
            std::span<const std::uint8_t> payload{};
        };

        struct QuicPacketProtectionResult {
            QuicPacketProtectionError error = QuicPacketProtectionError::None;
            std::size_t bytesWritten = 0;

            bool Succeeded() const noexcept {
                return error == QuicPacketProtectionError::None;
            }
        };

        // 检查非拥有的 packet protection 输入和输出边界，不接触密钥或密码库。
        LIKESPROGRAM_QUIC_API QuicPacketProtectionError
            ValidateQuicPacketProtectionRequest(
                const QuicPacketProtectionRequest& request,
                std::span<const std::uint8_t> output) noexcept;

        // 用户注入 TLS/QUIC 密码学实现；调用期间所有 span 由调用方保持有效。
        class LIKESPROGRAM_QUIC_API QuicPacketProtectionProvider {
        public:
            virtual ~QuicPacketProtectionProvider() = default;

            virtual QuicPacketProtectionResult Protect(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;

            virtual QuicPacketProtectionResult Unprotect(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;
        };

        enum class QuicPacketHeaderProtectionError : int {
            None = 0,
            InvalidInput = 1,
            OutputTooSmall = 2,
            NotReady = 3,
            AuthenticationFailed = 4,
            UnsupportedLevel = 5,
            InvalidPacketNumber = 6
        };

        struct QuicShortHeaderProtectionRequest {
            QuicPacketProtectionLevel level = QuicPacketProtectionLevel::OneRtt;
            std::uint64_t largestReceivedPacketNumber = 0;
            std::size_t destinationConnectionIdLength = 0;
            std::span<const std::uint8_t> packet{};
        };

        struct QuicShortHeaderProtectionResult {
            QuicPacketHeaderProtectionError error
                = QuicPacketHeaderProtectionError::None;
            std::size_t bytesWritten = 0;
            std::uint64_t packetNumber = 0;
            std::size_t packetNumberLength = 0;

            bool Succeeded() const noexcept {
                return error == QuicPacketHeaderProtectionError::None;
            }
        };

        struct QuicLongHeaderProtectionRequest {
            QuicPacketProtectionLevel level = QuicPacketProtectionLevel::Initial;
            std::uint64_t largestReceivedPacketNumber = 0;
            std::size_t packetNumberOffset = 0;
            // Protect requires 1..4. Unprotect may pass 0 so the provider
            // derives the length from the protected first byte.
            std::size_t packetNumberLength = 0;
            std::span<const std::uint8_t> packet{};
        };

        struct QuicLongHeaderProtectionResult {
            QuicPacketHeaderProtectionError error
                = QuicPacketHeaderProtectionError::None;
            std::size_t bytesWritten = 0;
            std::uint64_t packetNumber = 0;
            std::size_t packetNumberLength = 0;

            bool Succeeded() const noexcept {
                return error == QuicPacketHeaderProtectionError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicPacketHeaderProtectionError
            ValidateQuicLongHeaderProtectionRequest(
                const QuicLongHeaderProtectionRequest& request,
                std::span<const std::uint8_t> output) noexcept;

        LIKESPROGRAM_QUIC_API QuicPacketHeaderProtectionError
            ValidateQuicLongHeaderProtectionResult(
                const QuicLongHeaderProtectionRequest& request,
                const QuicLongHeaderProtectionResult& result,
                std::span<const std::uint8_t> output) noexcept;

        // Validates caller-owned short-header protection input and output size.
        LIKESPROGRAM_QUIC_API QuicPacketHeaderProtectionError
            ValidateQuicShortHeaderProtectionRequest(
                const QuicShortHeaderProtectionRequest& request,
                std::span<const std::uint8_t> output) noexcept;

        // Validates provider metadata after a complete short-header operation.
        LIKESPROGRAM_QUIC_API QuicPacketHeaderProtectionError
            ValidateQuicShortHeaderProtectionResult(
                const QuicShortHeaderProtectionRequest& request,
                const QuicShortHeaderProtectionResult& result,
                std::span<const std::uint8_t> output) noexcept;

        // The caller owns header protection, packet-number reconstruction and keys.
        // The provider only transforms supplied packet bytes and owns no TLS or UDP.
        class LIKESPROGRAM_QUIC_API QuicPacketHeaderProtectionProvider {
        public:
            virtual ~QuicPacketHeaderProtectionProvider() = default;

            virtual QuicShortHeaderProtectionResult ProtectShortHeader(
                const QuicShortHeaderProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;

            virtual QuicShortHeaderProtectionResult UnprotectShortHeader(
                const QuicShortHeaderProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;

            // Long-header protection is caller-owned. The default keeps
            // existing short-header-only providers source compatible and
            // reports that long-header protection is unavailable.
            virtual QuicLongHeaderProtectionResult ProtectLongHeader(
                const QuicLongHeaderProtectionRequest&,
                std::span<std::uint8_t>) noexcept {
                return { QuicPacketHeaderProtectionError::NotReady, 0, 0, 0 };
            }

            virtual QuicLongHeaderProtectionResult UnprotectLongHeader(
                const QuicLongHeaderProtectionRequest&,
                std::span<std::uint8_t>) noexcept {
                return { QuicPacketHeaderProtectionError::NotReady, 0, 0, 0 };
            }
        };
    }
}
