#include <LikesProgram/Quic/QuicCreditCongestionLedger.hpp>

#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicCreditCongestionLedgerTests() {
    using namespace LikesProgram::Quic;
    QuicCreditCongestionLedger ledger({ { 10, 4 }, 2, 2 });
    Require(ledger.SetCongestionWindow(10).IsOk(),
        "credit ledger should configure congestion window");
    Require(ledger.ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxData, 0, 10, 0 }).IsOk(),
        "credit ledger should accept MAX_DATA");
    Require(ledger.ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxStreamData, 7, 8, 0 }).IsOk(),
        "credit ledger should accept MAX_STREAM_DATA");
    Require(ledger.Reserve(1, 7, 4).IsOk(),
        "credit ledger should reserve all three budgets atomically");
    const auto reserved = ledger.Snapshot();
    Require(reserved.connection.reserved == 4
        && reserved.congestion.inFlightBytes == 4
        && reserved.reservations == 1,
        "reservation should update connection, congestion, and reservation snapshots");
    Require(!ledger.Reserve(2, 7, 5).IsOk(),
        "credit ledger should reject reservation above stream credit");
    const auto unchanged = ledger.Snapshot();
    Require(unchanged.connection.reserved == 4
        && unchanged.congestion.inFlightBytes == 4
        && unchanged.reservations == 1,
        "rejected reservation should preserve every budget");
    Require(ledger.Acknowledge(1).IsOk(),
        "credit ledger should release acknowledged reservation");
    Require(ledger.Snapshot().connection.reserved == 0
        && ledger.Snapshot().congestion.inFlightBytes == 0,
        "acknowledgement should clear connection and congestion reservations");

    Require(ledger.Reserve(3, 7, 6).IsOk(),
        "credit ledger should reserve after acknowledgement");
    Require(ledger.Lose(3).IsOk(),
        "credit ledger should release lost reservation");
    Require(!ledger.ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxData, 0, 9, 0 }).IsOk(),
        "credit ledger should reject connection credit regression");
    Require(!ledger.ApplyPeerFlowControl({
        QuicFlowControlFrameKind::DataBlocked, 0, 10, 0 }).IsOk(),
        "credit ledger should reject blocked policy frames");
    Require(ledger.StreamCredit(7).IsOk()
        && ledger.StreamCredit(7).Value().limit == 8,
        "credit ledger should expose stream credit snapshot");
    ledger.Reset();
    Require(ledger.Snapshot().trackedStreams == 0
        && ledger.Snapshot().reservations == 0
        && ledger.Snapshot().connection.limit == 0,
        "credit ledger reset should clear all accounting");
}
