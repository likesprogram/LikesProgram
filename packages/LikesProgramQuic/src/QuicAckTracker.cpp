#include <LikesProgram/Quic/QuicAckTracker.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicAckObservationResult ObserveQuicAck(
            QuicAckObservationState& state,
            const QuicAckFrame& frame,
            const QuicAckDelayContext& delayContext) noexcept {
            if (!DecodeQuicAckDelay(delayContext, frame.ackDelay).Succeeded()) {
                return { QuicAckObservationError::AckDelayInvalid, false,
                    state.largestObserved, state.largestObserved };
            }
            return ObserveQuicAck(state, frame);
        }

        QuicAckObservationResult ObserveQuicAck(
            QuicAckObservationState& state,
            const QuicAckFrame& frame) noexcept {
            if (frame.ranges.empty()) {
                return { QuicAckObservationError::EmptyRanges, false,
                    state.largestObserved, state.largestObserved };
            }
            if (frame.ranges.front().largest != frame.largestAcknowledged) {
                return { QuicAckObservationError::InvalidRange, false,
                    state.largestObserved, state.largestObserved };
            }
            for (std::size_t index = 0; index < frame.ranges.size(); ++index) {
                const auto& range = frame.ranges[index];
                if (range.smallest > range.largest) {
                    return { QuicAckObservationError::InvalidRange, false,
                        state.largestObserved, state.largestObserved };
                }
                if (index > 0) {
                    const auto& previous = frame.ranges[index - 1];
                    if (previous.smallest == 0
                        || range.largest >= previous.smallest - 1) {
                        return { QuicAckObservationError::InvalidOrder, false,
                            state.largestObserved, state.largestObserved };
                    }
                }
            }
            if (frame.ecn && state.hasEcnCounters
                && (frame.ect0Count < state.ect0Observed
                    || frame.ect1Count < state.ect1Observed
                    || frame.ecnCeCount < state.ecnCeObserved)) {
                return { QuicAckObservationError::EcnCounterRegression, false,
                    state.largestObserved, state.largestObserved };
            }
            if (state.observedFrames == std::numeric_limits<std::uint64_t>::max()) {
                return { QuicAckObservationError::CounterOverflow, false,
                    state.largestObserved, state.largestObserved };
            }
            const auto previousLargest = state.largestObserved;
            const bool advances = !state.hasLargest
                || frame.largestAcknowledged > state.largestObserved;
            state.hasLargest = true;
            if (advances) state.largestObserved = frame.largestAcknowledged;
            if (frame.ecn) {
                state.hasEcnCounters = true;
                state.ect0Observed = frame.ect0Count;
                state.ect1Observed = frame.ect1Count;
                state.ecnCeObserved = frame.ecnCeCount;
            }
            ++state.observedFrames;
            return { QuicAckObservationError::None, advances,
                previousLargest, state.largestObserved };
        }
    }
}
