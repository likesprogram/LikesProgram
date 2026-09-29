#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicConnectionCreditError {
            None,
            LimitRegression,
            InsufficientCredit,
            InvalidState,
        };

        struct QuicConnectionCreditState {
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

        struct QuicConnectionCreditResult {
            QuicConnectionCreditError error = QuicConnectionCreditError::None;
            std::uint64_t previous = 0;
            std::uint64_t current = 0;

            bool Succeeded() const noexcept {
                return error == QuicConnectionCreditError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicConnectionCreditResult ApplyQuicPeerConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t newLimit) noexcept;

        LIKESPROGRAM_QUIC_API QuicConnectionCreditResult ReserveQuicConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t bytes) noexcept;

        LIKESPROGRAM_QUIC_API QuicConnectionCreditResult ReleaseQuicConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t bytes) noexcept;
    }
}
