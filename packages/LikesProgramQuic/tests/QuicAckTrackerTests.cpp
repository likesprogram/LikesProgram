#include <LikesProgram/Quic/QuicAckTracker.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicAckTrackerTests() {
    using namespace LikesProgram::Quic;
    QuicAckObservationState state;
    QuicAckFrame first;
    first.largestAcknowledged = 10;
    first.ranges = { { 8, 10 }, { 2, 4 } };
    const auto observed = ObserveQuicAck(state, first);
    Require(observed.Succeeded() && observed.advancesLargest);
    Require(state.largestObserved == 10 && state.observedFrames == 1);

    QuicAckFrame older = first;
    older.largestAcknowledged = 9;
    older.ranges = { { 7, 9 } };
    const auto delayed = ObserveQuicAck(state, older);
    Require(delayed.Succeeded() && !delayed.advancesLargest);
    Require(state.largestObserved == 10 && state.observedFrames == 2);

    Require(ObserveQuicAck(state, QuicAckFrame{}).error
        == QuicAckObservationError::EmptyRanges);
    QuicAckFrame wrongLargest;
    wrongLargest.largestAcknowledged = 4;
    wrongLargest.ranges = { { 2, 3 } };
    Require(ObserveQuicAck(state, wrongLargest).error
        == QuicAckObservationError::InvalidRange);
    QuicAckFrame overlap;
    overlap.largestAcknowledged = 10;
    overlap.ranges = { { 8, 10 }, { 4, 8 } };
    Require(ObserveQuicAck(state, overlap).error
        == QuicAckObservationError::InvalidOrder);

    QuicAckFrame ecn = first;
    ecn.ecn = true;
    ecn.ect0Count = 4;
    ecn.ect1Count = 2;
    ecn.ecnCeCount = 1;
    Require(ObserveQuicAck(state, ecn).Succeeded()
        && state.hasEcnCounters
        && state.ect0Observed == 4
        && state.ect1Observed == 2
        && state.ecnCeObserved == 1);
    QuicAckFrame regressed = ecn;
    regressed.ect1Count = 1;
    Require(ObserveQuicAck(state, regressed).error
        == QuicAckObservationError::EcnCounterRegression
        && state.ect1Observed == 2);

    QuicAckDelayContext delayContext;
    Require(ConfigureQuicAckDelay(delayContext, 3, 80)
        == QuicAckDelayError::None);
    QuicAckFrame delayedFrame = first;
    delayedFrame.ackDelay = 10;
    const auto delayedWithContext = ObserveQuicAck(state, delayedFrame, delayContext);
    Require(delayedWithContext.Succeeded() && state.observedFrames == 4);
    delayedFrame.ackDelay = 11;
    const auto rejectedDelay = ObserveQuicAck(state, delayedFrame, delayContext);
    Require(rejectedDelay.error == QuicAckObservationError::AckDelayInvalid
        && state.observedFrames == 4);

    state.Reset();
    Require(!state.hasLargest && state.observedFrames == 0);
}
