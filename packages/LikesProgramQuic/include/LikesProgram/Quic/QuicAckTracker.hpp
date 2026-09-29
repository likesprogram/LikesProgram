#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicAckDelay.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>

#include <cstdint>
#include <limits>

namespace LikesProgram {
    namespace Quic {
        enum class QuicAckObservationError {
            None,
            EmptyRanges,
            InvalidRange,
            InvalidOrder,
            EcnCounterRegression,
            CounterOverflow,
            AckDelayInvalid,
        };

        struct QuicAckObservationState {
            bool hasLargest = false;
            std::uint64_t largestObserved = 0;
            std::uint64_t observedFrames = 0;
            bool hasEcnCounters = false;
            std::uint64_t ect0Observed = 0;
            std::uint64_t ect1Observed = 0;
            std::uint64_t ecnCeObserved = 0;

            void Reset() noexcept {
                hasLargest = false;
                largestObserved = 0;
                observedFrames = 0;
                hasEcnCounters = false;
                ect0Observed = 0;
                ect1Observed = 0;
                ecnCeObserved = 0;
            }
        };

        struct QuicAckObservationResult {
            QuicAckObservationError error = QuicAckObservationError::None;
            bool advancesLargest = false;
            std::uint64_t previousLargest = 0;
            std::uint64_t currentLargest = 0;

            bool Succeeded() const noexcept {
                return error == QuicAckObservationError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicAckObservationResult ObserveQuicAck(
            QuicAckObservationState& state,
            const QuicAckFrame& frame) noexcept;

        LIKESPROGRAM_QUIC_API QuicAckObservationResult ObserveQuicAck(
            QuicAckObservationState& state,
            const QuicAckFrame& frame,
            const QuicAckDelayContext& delayContext) noexcept;
    }
}
