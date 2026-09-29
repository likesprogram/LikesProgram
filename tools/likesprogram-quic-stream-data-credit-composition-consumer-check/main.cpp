#include <LikesProgram/Quic/QuicConnectionCredit.hpp>
#include <LikesProgram/Quic/QuicCreditReservation.hpp>
#include <LikesProgram/Quic/QuicStreamCredit.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    struct Ledger {
        QuicConnectionCreditState connection;
        QuicStreamCreditState stream;
        std::uint64_t committedOffset = 0;
        std::uint64_t nextActionId = 41;
    };

    struct PendingData {
        std::uint64_t actionId = 0;
        std::uint64_t offset = 0;
        std::uint64_t bytes = 0;
        std::vector<std::uint8_t> wire;
    };

    PendingData Prepare(Ledger& ledger, std::uint64_t streamId,
        std::span<const std::uint8_t> payload, bool fin) {
        const auto reservation = ReserveQuicCredit(
            ledger.connection, ledger.stream, payload.size());
        Require(reservation.Succeeded(), "DATA reservation should be atomic");

        const auto built = BuildQuicStreamFrame({
            streamId, ledger.committedOffset, true, true, fin, payload });
        if (!built.IsOk()) {
            const auto released = ReleaseQuicCredit(
                ledger.connection, ledger.stream, payload.size());
            Require(released.Succeeded(), "failed DATA build must release credit");
            throw std::runtime_error("DATA frame build failed");
        }
        return { ledger.nextActionId++, ledger.committedOffset,
            static_cast<std::uint64_t>(payload.size()), built.Value() };
    }

    void Reject(Ledger& ledger, const PendingData& pending) {
        const auto released = ReleaseQuicCredit(
            ledger.connection, ledger.stream, pending.bytes);
        Require(released.Succeeded(), "rejected DATA must release both ledgers");
    }

    void Accept(Ledger& ledger, const PendingData& pending,
        std::uint64_t streamId, bool fin) {
        const auto parsed = ParseQuicStreamFrame(
            pending.wire.data(), pending.wire.size());
        Require(parsed.IsOk() && parsed.Value().streamId == streamId
            && parsed.Value().offset == pending.offset
            && parsed.Value().hasOffset && parsed.Value().hasLength
            && parsed.Value().fin == fin
            && parsed.Value().data.size() == pending.bytes,
            "accepted DATA must preserve explicit wire coordinates");
        ledger.committedOffset += pending.bytes;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::uint64_t streamId = 8;
    const std::vector<std::uint8_t> firstPayload{ 0xa1, 0xb2, 0xc3 };
    const std::vector<std::uint8_t> secondPayload{ 0xd4, 0xe5, 0xf6, 0x07 };

    Ledger ledger;
    Require(ApplyQuicPeerConnectionCredit(ledger.connection, 7).Succeeded()
        && ApplyQuicPeerStreamCredit(ledger.stream, 7).Succeeded(),
        "peer limits should initialize both caller-owned ledgers");

    const auto connectionRegression = ApplyQuicPeerConnectionCredit(
        ledger.connection, 6);
    const auto streamRegression = ApplyQuicPeerStreamCredit(ledger.stream, 6);
    Require(connectionRegression.error == QuicConnectionCreditError::LimitRegression
        && streamRegression.error == QuicStreamCreditError::LimitRegression
        && ledger.connection.limit == 7 && ledger.stream.limit == 7,
        "limit regressions must preserve both limits");

    auto rejected = Prepare(ledger, streamId, firstPayload, false);
    Require(rejected.actionId != 0 && rejected.offset == 0
        && ledger.connection.reserved == firstPayload.size()
        && ledger.stream.reserved == firstPayload.size(),
        "prepared DATA must reserve exact bytes and keep action id external");
    const auto rejectedConnectionBefore = ledger.connection;
    const auto rejectedStreamBefore = ledger.stream;
    Reject(ledger, rejected);
    Require(ledger.connection.reserved == rejectedConnectionBefore.reserved
        - rejected.bytes && ledger.stream.reserved == rejectedStreamBefore.reserved
        - rejected.bytes && ledger.committedOffset == 0,
        "rejection must roll back reservation without committing offset");

    auto accepted = Prepare(ledger, streamId, firstPayload, false);
    Accept(ledger, accepted, streamId, false);
    Require(ledger.committedOffset == firstPayload.size()
        && ledger.connection.reserved == firstPayload.size()
        && ledger.stream.reserved == firstPayload.size(),
        "accepted DATA must commit offset while retaining reservation");

    auto malformed = Prepare(ledger, streamId, secondPayload, true);
    const auto connectionBeforeMalformed = ledger.connection;
    const auto streamBeforeMalformed = ledger.stream;
    const auto offsetBeforeMalformed = ledger.committedOffset;
    std::vector<std::uint8_t> truncated(malformed.wire.begin(),
        malformed.wire.end() - 1);
    Require(!ParseQuicStreamFrame(truncated.data(), truncated.size()).IsOk(),
        "truncated DATA must be rejected");
    Require(ledger.connection.limit == connectionBeforeMalformed.limit
        && ledger.connection.reserved == connectionBeforeMalformed.reserved
        && ledger.stream.limit == streamBeforeMalformed.limit
        && ledger.stream.reserved == streamBeforeMalformed.reserved
        && ledger.committedOffset == offsetBeforeMalformed,
        "malformed input must not mutate caller ledgers");
    Reject(ledger, malformed);
    Require(ledger.committedOffset == firstPayload.size()
        && ledger.connection.reserved == firstPayload.size()
        && ledger.stream.reserved == firstPayload.size(),
        "malformed DATA rejection must release only its reservation");

    auto finalData = Prepare(ledger, streamId, secondPayload, true);
    Accept(ledger, finalData, streamId, true);
    Require(ledger.committedOffset == firstPayload.size() + secondPayload.size()
        && ledger.connection.reserved == 7 && ledger.stream.reserved == 7,
        "FIN DATA must preserve final offset and both reservations");

    Ledger streamShortageLedger;
    streamShortageLedger.connection.limit = 8;
    streamShortageLedger.connection.reserved = 7;
    streamShortageLedger.stream.limit = 7;
    streamShortageLedger.stream.reserved = 7;
    const auto streamShortage = ReserveQuicCredit(
        streamShortageLedger.connection, streamShortageLedger.stream, 1);
    Require(streamShortage.error == QuicCreditReservationError::StreamInsufficient
        && streamShortageLedger.connection.reserved == 7
        && streamShortageLedger.stream.reserved == 7,
        "stream shortage must preserve connection and stream state");

    Ledger invalid;
    invalid.connection.limit = 2;
    invalid.connection.reserved = 3;
    invalid.stream.limit = 8;
    const auto invalidBefore = invalid;
    const auto invalidResult = ReserveQuicCredit(
        invalid.connection, invalid.stream, 1);
    Require(invalidResult.error == QuicCreditReservationError::ConnectionInvalidState
        && invalid.connection.limit == invalidBefore.connection.limit
        && invalid.connection.reserved == invalidBefore.connection.reserved
        && invalid.stream.limit == invalidBefore.stream.limit
        && invalid.stream.reserved == invalidBefore.stream.reserved,
        "invalid state must reject without mutation");

    ledger.connection.Reset();
    ledger.stream.Reset();
    Require(ledger.connection.reserved == 0 && ledger.stream.reserved == 0,
        "Reset must clear both caller-owned credit ledgers");

    std::cout << "passed=true"
              << " data_frame=true"
              << " credit_atomic=true"
              << " rejection_rollback=true"
              << " acceptance_commit=true"
              << " fin_coordinate=true"
              << " invalid_preserves=true"
              << " action_id_external=true\n";
    return 0;
}

int main() {
    try {
        return Run();
    }
    catch (const std::exception& error) {
        std::cerr << "failed=" << error.what() << '\n';
        return 99;
    }
}
