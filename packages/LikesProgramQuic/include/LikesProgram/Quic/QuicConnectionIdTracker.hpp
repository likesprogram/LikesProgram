#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        enum class QuicConnectionIdObservationError {
            None,
            SequenceRegression,
            RetirePriorToRegression,
            RetireBeyondObserved,
        };

        struct QuicConnectionIdObservationState {
            bool hasIssuedSequence = false;
            std::uint64_t highestIssuedSequence = 0;
            std::uint64_t retirePriorTo = 0;
            bool hasRetiredSequence = false;
            std::uint64_t highestRetiredSequence = 0;

            void Reset() noexcept {
                hasIssuedSequence = false;
                highestIssuedSequence = 0;
                retirePriorTo = 0;
                hasRetiredSequence = false;
                highestRetiredSequence = 0;
            }
        };

        struct QuicConnectionIdObservationResult {
            QuicConnectionIdObservationError error = QuicConnectionIdObservationError::None;
            bool advanced = false;

            bool Succeeded() const noexcept {
                return error == QuicConnectionIdObservationError::None;
            }
        };

        LIKESPROGRAM_QUIC_API QuicConnectionIdObservationResult ObserveQuicConnectionIdFrame(
            QuicConnectionIdObservationState& state,
            const QuicConnectionIdFrame& frame) noexcept;
    }
}
