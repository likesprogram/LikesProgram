#include <LikesProgram/Quic/QuicStreamCredit.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicStreamCreditResult ApplyQuicPeerStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t newLimit) noexcept {
            if (state.reserved > state.limit) {
                return { QuicStreamCreditError::InvalidState, state.limit, state.limit };
            }
            if (newLimit < state.limit) {
                return { QuicStreamCreditError::LimitRegression, state.limit, newLimit };
            }
            const auto previous = state.limit;
            state.limit = newLimit;
            return { QuicStreamCreditError::None, previous, state.limit };
        }

        QuicStreamCreditResult ReserveQuicStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t bytes) noexcept {
            if (state.reserved > state.limit) {
                return { QuicStreamCreditError::InvalidState, state.reserved, state.reserved };
            }
            if (bytes > state.limit - state.reserved) {
                return { QuicStreamCreditError::InsufficientCredit,
                    state.reserved, state.reserved };
            }
            const auto previous = state.reserved;
            state.reserved += bytes;
            return { QuicStreamCreditError::None, previous, state.reserved };
        }

        QuicStreamCreditResult ReleaseQuicStreamCredit(
            QuicStreamCreditState& state,
            std::uint64_t bytes) noexcept {
            if (state.reserved > state.limit) {
                return { QuicStreamCreditError::InvalidState, state.reserved, state.reserved };
            }
            if (bytes > state.reserved) {
                return { QuicStreamCreditError::InsufficientCredit,
                    state.reserved, state.reserved };
            }
            const auto previous = state.reserved;
            state.reserved -= bytes;
            return { QuicStreamCreditError::None, previous, state.reserved };
        }
    }
}
