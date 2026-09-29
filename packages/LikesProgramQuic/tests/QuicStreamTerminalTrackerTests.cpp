#include <LikesProgram/Quic/QuicStreamTerminalTracker.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicStreamTerminalTrackerTests() {
    using namespace LikesProgram::Quic;
    QuicStreamTerminalState state;
    Require(ObserveQuicStreamFin(state, 12).advanced);
    Require(ObserveQuicStreamFin(state, 12).Succeeded());
    Require(!ObserveQuicStreamFin(state, 12).advanced);
    Require(ObserveQuicStreamFin(state, 13).error
        == QuicStreamTerminalError::FinalSizeMismatch);
    Require(ObserveQuicStreamReset(state, 1, 12).error
        == QuicStreamTerminalError::ResetAfterFin);

    state.Reset();
    Require(ObserveQuicStreamReset(state, 7, 24).advanced);
    Require(ObserveQuicStreamReset(state, 7, 24).Succeeded());
    Require(ObserveQuicStreamReset(state, 8, 24).error
        == QuicStreamTerminalError::ResetCodeMismatch);
    Require(ObserveQuicStreamReset(state, 7, 25).error
        == QuicStreamTerminalError::FinalSizeMismatch);
    Require(ObserveQuicStreamFin(state, 24).error
        == QuicStreamTerminalError::FinAfterReset);

    state.Reset();
    Require(ObserveQuicStreamStopSending(state, 9).advanced);
    Require(ObserveQuicStreamStopSending(state, 9).Succeeded());
    Require(ObserveQuicStreamStopSending(state, 10).error
        == QuicStreamTerminalError::StopSendingCodeMismatch);
    Require(ObserveQuicStreamFin(state, kQuicVarIntMaximum).advanced);
    Require(ObserveQuicStreamFin(state, kQuicVarIntMaximum + 1).error
        == QuicStreamTerminalError::InvalidFinalSize);
    state.Reset();
    Require(!state.finObserved && !state.resetObserved && !state.stopSendingObserved);
}
