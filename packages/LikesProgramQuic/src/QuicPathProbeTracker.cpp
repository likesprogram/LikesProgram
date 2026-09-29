#include <LikesProgram/Quic/QuicPathProbeTracker.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicPathProbeResult ObserveQuicPathChallenge(
            QuicPathProbeState& state,
            const std::array<std::uint8_t, 8>& token) noexcept {
            const bool advanced = !state.pending || state.token != token;
            state.pending = true;
            state.validated = false;
            state.token = token;
            return { QuicPathProbeError::None, advanced };
        }

        QuicPathProbeResult ObserveQuicPathResponse(
            QuicPathProbeState& state,
            const std::array<std::uint8_t, 8>& token) noexcept {
            if (!state.pending) {
                return { QuicPathProbeError::ResponseWithoutChallenge, false };
            }
            if (state.token != token) {
                return { QuicPathProbeError::TokenMismatch, false };
            }
            state.pending = false;
            state.validated = true;
            return { QuicPathProbeError::None, true };
        }
    }
}
