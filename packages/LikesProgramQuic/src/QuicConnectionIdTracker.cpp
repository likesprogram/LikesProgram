#include <LikesProgram/Quic/QuicConnectionIdTracker.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicConnectionIdObservationResult ObserveQuicConnectionIdFrame(
            QuicConnectionIdObservationState& state,
            const QuicConnectionIdFrame& frame) noexcept {
            if (frame.kind == QuicConnectionIdFrameKind::NewConnectionId) {
                if (state.hasIssuedSequence
                    && frame.sequenceNumber < state.highestIssuedSequence) {
                    return { QuicConnectionIdObservationError::SequenceRegression, false };
                }
                if (frame.retirePriorTo < state.retirePriorTo) {
                    return { QuicConnectionIdObservationError::RetirePriorToRegression, false };
                }
                if (frame.retirePriorTo > frame.sequenceNumber) {
                    return { QuicConnectionIdObservationError::RetireBeyondObserved, false };
                }
                const bool advances = !state.hasIssuedSequence
                    || frame.sequenceNumber > state.highestIssuedSequence
                    || frame.retirePriorTo > state.retirePriorTo;
                state.hasIssuedSequence = true;
                if (frame.sequenceNumber > state.highestIssuedSequence) {
                    state.highestIssuedSequence = frame.sequenceNumber;
                }
                if (frame.retirePriorTo > state.retirePriorTo) {
                    state.retirePriorTo = frame.retirePriorTo;
                }
                return { QuicConnectionIdObservationError::None, advances };
            }

            if (!state.hasIssuedSequence || frame.sequenceNumber > state.highestIssuedSequence) {
                return { QuicConnectionIdObservationError::RetireBeyondObserved, false };
            }
            const bool advances = !state.hasRetiredSequence
                || frame.sequenceNumber > state.highestRetiredSequence;
            state.hasRetiredSequence = true;
            if (frame.sequenceNumber > state.highestRetiredSequence) {
                state.highestRetiredSequence = frame.sequenceNumber;
            }
            return { QuicConnectionIdObservationError::None, advances };
        }
    }
}
