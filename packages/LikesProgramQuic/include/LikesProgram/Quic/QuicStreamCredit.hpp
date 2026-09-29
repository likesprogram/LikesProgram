#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <cstdint>
#include <limits>

namespace LikesProgram {
    namespace Quic {
        enum class QuicStreamCreditError {
            None,
            LimitRegression,
            InsufficientCredit,
            InvalidState,
        };

        struct QuicStreamCreditState {
            std::uint64_t limit = 0;
            std::uint64_t reserved = 0;

            std::uint64_t Available() const noexcept {
                return reserved <= limit ? limit - reserved : 0;
            }

            void Reset() noexcept {
                limit = 0;
                reserved = 0;
            }
        };

        struct QuicStreamCreditResult {
            QuicStreamCreditError error = QuicStreamCreditError::None;
            std::uint64_t previous = 0;
            std::uint64_t current = 0;

            bool Succeeded() const noexcept {
                return error == QuicStreamCreditError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicStreamCreditResult ApplyQuicPeerStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t newLimit) noexcept;

        LIKESPROGRAM_QUIC_API QuicStreamCreditResult ReserveQuicStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t bytes) noexcept;

        LIKESPROGRAM_QUIC_API QuicStreamCreditResult ReleaseQuicStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t bytes) noexcept;
    }
}
