#include <LikesProgram/Quic/QuicStreamTerminalTracker.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicStreamTerminalResult ObserveQuicStreamFin(
            QuicStreamTerminalState& state,
            std::uint64_t finalSize) noexcept {
            if (finalSize > kQuicVarIntMaximum) {
                return { QuicStreamTerminalError::InvalidFinalSize, false };
            }
            if (state.resetObserved) {
                return { QuicStreamTerminalError::FinAfterReset, false };
            }
            if (state.finObserved) {
                if (state.finalSize != finalSize) {
                    return { QuicStreamTerminalError::FinalSizeMismatch, false };
                }
                return { QuicStreamTerminalError::None, false };
            }
            state.finObserved = true;
            state.finalSize = finalSize;
            return { QuicStreamTerminalError::None, true };
        }

        QuicStreamTerminalResult ObserveQuicStreamReset(
            QuicStreamTerminalState& state,
            std::uint64_t errorCode,
            std::uint64_t finalSize) noexcept {
            if (finalSize > kQuicVarIntMaximum) {
                return { QuicStreamTerminalError::InvalidFinalSize, false };
            }
            if (state.finObserved) {
                return { QuicStreamTerminalError::ResetAfterFin, false };
            }
            if (state.resetObserved) {
                if (state.finalSize != finalSize) {
                    return { QuicStreamTerminalError::FinalSizeMismatch, false };
                }
                if (state.resetErrorCode != errorCode) {
                    return { QuicStreamTerminalError::ResetCodeMismatch, false };
                }
                return { QuicStreamTerminalError::None, false };
            }
            state.resetObserved = true;
            state.resetErrorCode = errorCode;
            state.finalSize = finalSize;
            return { QuicStreamTerminalError::None, true };
        }

        QuicStreamTerminalResult ObserveQuicStreamStopSending(
            QuicStreamTerminalState& state,
            std::uint64_t errorCode) noexcept {
            if (state.stopSendingObserved) {
                if (state.stopSendingErrorCode != errorCode) {
                    return { QuicStreamTerminalError::StopSendingCodeMismatch, false };
                }
                return { QuicStreamTerminalError::None, false };
            }
            state.stopSendingObserved = true;
            state.stopSendingErrorCode = errorCode;
            return { QuicStreamTerminalError::None, true };
        }
    }
}
