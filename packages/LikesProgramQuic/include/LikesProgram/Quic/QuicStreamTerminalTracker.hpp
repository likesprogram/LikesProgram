#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicStreamTerminalError {
            None,
            InvalidFinalSize,
            FinAfterReset,
            ResetAfterFin,
            FinalSizeMismatch,
            ResetCodeMismatch,
            StopSendingCodeMismatch,
        };

        struct QuicStreamTerminalState {
            bool finObserved = false;
            bool resetObserved = false;
            bool stopSendingObserved = false;
            std::uint64_t finalSize = 0;
            std::uint64_t resetErrorCode = 0;
            std::uint64_t stopSendingErrorCode = 0;

            void Reset() noexcept {
                finObserved = false;
                resetObserved = false;
                stopSendingObserved = false;
                finalSize = 0;
                resetErrorCode = 0;
                stopSendingErrorCode = 0;
            }
        };

        struct QuicStreamTerminalResult {
            QuicStreamTerminalError error = QuicStreamTerminalError::None;
            bool advanced = false;

            bool Succeeded() const noexcept {
                return error == QuicStreamTerminalError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicStreamTerminalResult ObserveQuicStreamFin(
            QuicStreamTerminalState& state,
            std::uint64_t finalSize) noexcept;

        LIKESPROGRAM_QUIC_API QuicStreamTerminalResult ObserveQuicStreamReset(
            QuicStreamTerminalState& state,
            std::uint64_t errorCode,
            std::uint64_t finalSize) noexcept;

        LIKESPROGRAM_QUIC_API QuicStreamTerminalResult ObserveQuicStreamStopSending(
            QuicStreamTerminalState& state,
            std::uint64_t errorCode) noexcept;
    }
}
