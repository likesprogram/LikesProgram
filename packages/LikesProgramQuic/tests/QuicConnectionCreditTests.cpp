#include <LikesProgram/Quic/QuicConnectionCredit.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicConnectionCreditTests() {
    using namespace LikesProgram::Quic;
    QuicConnectionCreditState connection;
    Require(ApplyQuicPeerConnectionCredit(connection, 4096).Succeeded());
    Require(connection.Available() == 4096);
    Require(ReserveQuicConnectionCredit(connection, 1024).Succeeded());
    Require(connection.reserved == 1024);
    Require(ReleaseQuicConnectionCredit(connection, 512).Succeeded());
    Require(connection.reserved == 512);
    Require(ReserveQuicConnectionCredit(connection, 3585).error
        == QuicConnectionCreditError::InsufficientCredit);
    Require(ApplyQuicPeerConnectionCredit(connection, 2048).error
        == QuicConnectionCreditError::LimitRegression);

    connection.limit = 3;
    connection.reserved = 4;
    Require(ReleaseQuicConnectionCredit(connection, 1).error
        == QuicConnectionCreditError::InvalidState);
    connection.Reset();
    Require(connection.Available() == 0);
}
