#include <LikesProgram/Quic/QuicCreditReservation.hpp>

#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicCreditReservationTests() {
    using namespace LikesProgram::Quic;
    QuicConnectionCreditState connection{ 100, 10 };
    QuicStreamCreditState stream{ 50, 5 };
    Require(ReserveQuicCredit(connection, stream, 20).Succeeded());
    Require(connection.reserved == 30 && stream.reserved == 25);
    Require(ReleaseQuicCredit(connection, stream, 10).Succeeded());
    Require(connection.reserved == 20 && stream.reserved == 15);

    const auto connectionShort = ReserveQuicCredit(connection, stream, 81);
    Require(connectionShort.error == QuicCreditReservationError::ConnectionInsufficient);
    Require(connection.reserved == 20 && stream.reserved == 15);
    const auto streamShort = ReserveQuicCredit(connection, stream, 36);
    Require(streamShort.error == QuicCreditReservationError::StreamInsufficient);
    Require(connection.reserved == 20 && stream.reserved == 15);

    stream.limit = 1;
    stream.reserved = 2;
    Require(ReserveQuicCredit(connection, stream, 1).error
        == QuicCreditReservationError::StreamInvalidState);
    stream.Reset();
    Require(ReleaseQuicCredit(connection, stream, 1).error
        == QuicCreditReservationError::ReleaseInsufficient);
}
