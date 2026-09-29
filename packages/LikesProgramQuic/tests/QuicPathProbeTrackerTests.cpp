#include <LikesProgram/Quic/QuicPathProbeTracker.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicPathProbeTrackerTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 8> first{ 0, 1, 2, 3, 4, 5, 6, 7 };
    const std::array<std::uint8_t, 8> other{ 7, 6, 5, 4, 3, 2, 1, 0 };
    QuicPathProbeState state;
    Require(ObserveQuicPathResponse(state, first).error
        == QuicPathProbeError::ResponseWithoutChallenge);
    Require(ObserveQuicPathChallenge(state, first).advanced);
    Require(ObserveQuicPathChallenge(state, first).Succeeded());
    Require(ObserveQuicPathResponse(state, other).error
        == QuicPathProbeError::TokenMismatch);
    Require(state.pending && !state.validated);
    Require(ObserveQuicPathResponse(state, first).advanced);
    Require(!state.pending && state.validated);
    Require(ObserveQuicPathChallenge(state, other).advanced);
    state.Reset();
    Require(!state.pending && !state.validated);
}
