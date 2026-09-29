#include <LikesProgram/Quic/QuicConnectionCredit.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicConnectionCreditResult ApplyQuicPeerConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t newLimit) noexcept {
            if (state.reserved > state.limit) {
                return { QuicConnectionCreditError::InvalidState, state.limit, state.limit };
            }
            if (newLimit < state.limit) {
                return { QuicConnectionCreditError::LimitRegression,
                    state.limit, newLimit };
            }
            const auto previous = state.limit;
            state.limit = newLimit;
            return { QuicConnectionCreditError::None, previous, state.limit };
        }

        QuicConnectionCreditResult ReserveQuicConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t bytes) noexcept {
            if (state.reserved > state.limit) {
                return { QuicConnectionCreditError::InvalidState,
                    state.reserved, state.reserved };
            }
            if (bytes > state.limit - state.reserved) {
                return { QuicConnectionCreditError::InsufficientCredit,
                    state.reserved, state.reserved };
            }
            const auto previous = state.reserved;
            state.reserved += bytes;
            return { QuicConnectionCreditError::None, previous, state.reserved };
        }

        QuicConnectionCreditResult ReleaseQuicConnectionCredit(
            QuicConnectionCreditState& state,
            std::uint64_t bytes) noexcept {
            if (state.reserved > state.limit) {
                return { QuicConnectionCreditError::InvalidState,
                    state.reserved, state.reserved };
            }
            if (bytes > state.reserved) {
                return { QuicConnectionCreditError::InsufficientCredit,
                    state.reserved, state.reserved };
            }
            const auto previous = state.reserved;
            state.reserved -= bytes;
            return { QuicConnectionCreditError::None, previous, state.reserved };
        }
    }
}
