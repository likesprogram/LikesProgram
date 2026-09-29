#include <LikesProgram/Quic/QuicStreamCredit.hpp>

#include <cstdlib>
#include <limits>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicStreamCreditTests() {
    using namespace LikesProgram::Quic;
    QuicStreamCreditState state;
    Require(ApplyQuicPeerStreamCredit(state, 1024).Succeeded());
    Require(state.limit == 1024 && state.Available() == 1024);
    Require(ApplyQuicPeerStreamCredit(state, 1024).Succeeded());
    const auto regression = ApplyQuicPeerStreamCredit(state, 512);
    Require(regression.error == QuicStreamCreditError::LimitRegression);
    Require(state.limit == 1024);

    Require(ReserveQuicStreamCredit(state, 256).Succeeded());
    Require(state.reserved == 256 && state.Available() == 768);
    const auto insufficient = ReserveQuicStreamCredit(state, 769);
    Require(insufficient.error == QuicStreamCreditError::InsufficientCredit);
    Require(state.reserved == 256);
    Require(ReleaseQuicStreamCredit(state, 128).Succeeded());
    Require(state.reserved == 128);
    Require(ReleaseQuicStreamCredit(state, 129).error
        == QuicStreamCreditError::InsufficientCredit);

    state.limit = std::numeric_limits<std::uint64_t>::max();
    state.reserved = 0;
    Require(ReserveQuicStreamCredit(state,
        std::numeric_limits<std::uint64_t>::max()).Succeeded());
    Require(state.Available() == 0);
    Require(ReserveQuicStreamCredit(state, 1).error
        == QuicStreamCreditError::InsufficientCredit);

    state.limit = 1;
    state.reserved = 2;
    Require(ApplyQuicPeerStreamCredit(state, 3).error
        == QuicStreamCreditError::InvalidState);
    state.Reset();
    Require(state.limit == 0 && state.reserved == 0);
}
