#include <LikesProgram/Quic/QuicConnectionIdTracker.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicConnectionIdTrackerTests() {
    using namespace LikesProgram::Quic;
    QuicConnectionIdObservationState state;
    const QuicConnectionIdFrame first{
        QuicConnectionIdFrameKind::NewConnectionId, 0, 0, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, first).advanced);
    const QuicConnectionIdFrame next{
        QuicConnectionIdFrameKind::NewConnectionId, 1, 1, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, next).advanced);
    Require(ObserveQuicConnectionIdFrame(state, next).Succeeded());

    const QuicConnectionIdFrame regression{
        QuicConnectionIdFrameKind::NewConnectionId, 0, 1, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, regression).error
        == QuicConnectionIdObservationError::SequenceRegression);
    const QuicConnectionIdFrame beyond{
        QuicConnectionIdFrameKind::NewConnectionId, 2, 3, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, beyond).error
        == QuicConnectionIdObservationError::RetireBeyondObserved);

    const QuicConnectionIdFrame retire{
        QuicConnectionIdFrameKind::RetireConnectionId, 1, 0, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, retire).advanced);
    Require(ObserveQuicConnectionIdFrame(state, retire).Succeeded());
    const QuicConnectionIdFrame retireBeyond{
        QuicConnectionIdFrameKind::RetireConnectionId, 9, 0, {}, {}, 0 };
    Require(ObserveQuicConnectionIdFrame(state, retireBeyond).error
        == QuicConnectionIdObservationError::RetireBeyondObserved);
    state.Reset();
    Require(!state.hasIssuedSequence && !state.hasRetiredSequence);
}
