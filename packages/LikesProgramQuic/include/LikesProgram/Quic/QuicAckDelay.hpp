#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicAckDelayError {
            None,
            NotConfigured,
            InvalidExponent,
            Overflow,
            ExceedsMaximum,
        };

        struct QuicAckDelayContext {
            static constexpr std::uint8_t kMaxExponent = 20;

            std::uint8_t exponent = 3;
            std::uint64_t maxDelayMicroseconds = 0;
            bool configured = false;

            void Reset() noexcept {
                exponent = 3;
                maxDelayMicroseconds = 0;
                configured = false;
            }
        };

        struct QuicAckDelayResult {
            QuicAckDelayError error = QuicAckDelayError::None;
            std::uint64_t delayMicroseconds = 0;

            bool Succeeded() const noexcept {
                return error == QuicAckDelayError::None;
            }
        };

        // Stores caller-supplied transport parameters without reading a clock
        // or owning PTO/loss-recovery state.
        LIKESPROGRAM_QUIC_API QuicAckDelayError ConfigureQuicAckDelay(
            QuicAckDelayContext& context,
            std::uint8_t exponent,
            std::uint64_t maxDelayMicroseconds) noexcept;

        // Converts raw ACK-frame delay by 2^exponent and bounds it by the
        // caller-supplied maximum. No timer or RTT policy is applied here.
        LIKESPROGRAM_QUIC_API QuicAckDelayResult DecodeQuicAckDelay(
            const QuicAckDelayContext& context,
            std::uint64_t encodedDelay) noexcept;
    }
}
