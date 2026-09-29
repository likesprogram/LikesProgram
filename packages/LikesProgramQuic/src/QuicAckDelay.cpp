#include <LikesProgram/Quic/QuicAckDelay.hpp>

#include <limits>

namespace LikesProgram {
    namespace Quic {
        QuicAckDelayError ConfigureQuicAckDelay(
            QuicAckDelayContext& context,
            std::uint8_t exponent,
            std::uint64_t maxDelayMicroseconds) noexcept {
            if (exponent > QuicAckDelayContext::kMaxExponent) {
                return QuicAckDelayError::InvalidExponent;
            }
            context.exponent = exponent;
            context.maxDelayMicroseconds = maxDelayMicroseconds;
            context.configured = true;
            return QuicAckDelayError::None;
        }

        QuicAckDelayResult DecodeQuicAckDelay(
            const QuicAckDelayContext& context,
            std::uint64_t encodedDelay) noexcept {
            if (!context.configured) {
                return { QuicAckDelayError::NotConfigured, 0 };
            }
            if (context.exponent > QuicAckDelayContext::kMaxExponent) {
                return { QuicAckDelayError::InvalidExponent, 0 };
            }
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (encodedDelay > (maximum >> context.exponent)) {
                return { QuicAckDelayError::Overflow, 0 };
            }
            const auto delay = encodedDelay << context.exponent;
            if (delay > context.maxDelayMicroseconds) {
                return { QuicAckDelayError::ExceedsMaximum, delay };
            }
            return { QuicAckDelayError::None, delay };
        }
    }
}
