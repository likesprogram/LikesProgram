#include <LikesProgram/Quic/QuicPathMigrationLedger.hpp>

#include <array>
#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicPathMigrationLedgerTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 8> token{ 0, 1, 2, 3, 4, 5, 6, 7 };
    const QuicPathValidationFrame challenge{
        QuicPathValidationFrameKind::PathChallenge, token, 0 };
    QuicPathMigrationLedger ledger;

    const auto response = ledger.ObserveResponse({
        QuicPathValidationFrameKind::PathResponse, token, 0 });
    Require(!response.IsOk());
    const auto challengeResult = ledger.ObserveChallenge(challenge);
    Require(challengeResult.IsOk() && challengeResult.Value().probe.advanced
        && challengeResult.Value().response.kind == QuicActionKind::PathResponse
        && challengeResult.Value().response.actionId == 0);
    const auto responseFrame = ParseQuicPathValidationFrame(
        challengeResult.Value().response.payload.Peek(),
        challengeResult.Value().response.payload.ReadableBytes());
    Require(responseFrame.IsOk()
        && responseFrame.Value().kind == QuicPathValidationFrameKind::PathResponse
        && responseFrame.Value().data == token);
    const auto accepted = ledger.ObserveResponse(responseFrame.Value());
    Require(accepted.IsOk() && accepted.Value().advanced
        && ledger.Snapshot().probe.validated);
    const auto mismatch = ledger.ObserveResponse({
        QuicPathValidationFrameKind::PathResponse, std::array<std::uint8_t, 8>{ 9 }, 0 });
    Require(!mismatch.IsOk() && ledger.Snapshot().probe.validated);

    QuicPathMigrationLedger initiated;
    Require(initiated.BeginValidation(token).IsOk());
    Require(initiated.Snapshot().probe.pending
        && initiated.Snapshot().probe.token == token);
    Require(!initiated.ObserveResponse({
        QuicPathValidationFrameKind::PathResponse,
        std::array<std::uint8_t, 8>{ 9 },
        0
    }).IsOk());
    Require(initiated.Snapshot().probe.pending
        && !initiated.Snapshot().probe.validated);
    Require(initiated.ObserveResponse({
        QuicPathValidationFrameKind::PathResponse, token, 0 }).IsOk());
    Require(!initiated.Snapshot().probe.pending
        && initiated.Snapshot().probe.validated);
    Require(initiated.BeginValidation(token).IsOk());
    initiated.CancelValidation();
    Require(!initiated.Snapshot().probe.pending
        && !initiated.Snapshot().probe.validated);

    const QuicConnectionIdFrame first{
        QuicConnectionIdFrameKind::NewConnectionId, 0, 0, {}, {}, 0 };
    Require(ledger.ObserveConnectionId(first).IsOk());
    const QuicConnectionIdFrame next{
        QuicConnectionIdFrameKind::NewConnectionId, 1, 1, {}, {}, 0 };
    Require(ledger.ObserveConnectionId(next).IsOk());
    const QuicConnectionIdFrame regression{
        QuicConnectionIdFrameKind::NewConnectionId, 0, 1, {}, {}, 0 };
    Require(!ledger.ObserveConnectionId(regression).IsOk()
        && ledger.Snapshot().connectionId.highestIssuedSequence == 1);
    const QuicConnectionIdFrame retire{
        QuicConnectionIdFrameKind::RetireConnectionId, 1, 0, {}, {}, 0 };
    Require(ledger.ObserveConnectionId(retire).IsOk()
        && ledger.Snapshot().connectionId.highestRetiredSequence == 1);
    ledger.Reset();
    Require(!ledger.Snapshot().probe.pending
        && !ledger.Snapshot().connectionId.hasIssuedSequence);
}
