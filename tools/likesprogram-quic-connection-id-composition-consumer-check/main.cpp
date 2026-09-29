#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionIdTracker.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool SameBytes(
        const std::span<const std::uint8_t> actual,
        const std::span<const std::uint8_t> expected) {
        return actual.size() == expected.size()
            && std::equal(actual.begin(), actual.end(), expected.begin());
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 4> connectionId{ 0x10, 0x20, 0x30, 0x40 };
    const std::array<std::uint8_t, 16> resetToken{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const QuicConnectionIdFrame issued{
        QuicConnectionIdFrameKind::NewConnectionId,
        9,
        3,
        connectionId,
        resetToken,
        0 };

    const auto encodedIssued = BuildQuicConnectionIdFrame(issued);
    Require(encodedIssued.IsOk(), "NEW_CONNECTION_ID should build");
    auto issuedBytes = encodedIssued.Value();
    issuedBytes.push_back(0xA5);
    const auto parsedIssued = ParseQuicConnectionIdFrame(
        issuedBytes.data(), issuedBytes.size());
    Require(parsedIssued.IsOk()
        && parsedIssued.Value().kind == QuicConnectionIdFrameKind::NewConnectionId
        && parsedIssued.Value().sequenceNumber == issued.sequenceNumber
        && parsedIssued.Value().retirePriorTo == issued.retirePriorTo
        && SameBytes(parsedIssued.Value().connectionId, connectionId)
        && SameBytes(parsedIssued.Value().statelessResetToken, resetToken)
        && parsedIssued.Value().consumedBytes == encodedIssued.Value().size(),
        "NEW_CONNECTION_ID should preserve fields and consumed boundary");

    QuicConnectionIdObservationState state;
    const auto first = ObserveQuicConnectionIdFrame(state, parsedIssued.Value());
    const auto duplicate = ObserveQuicConnectionIdFrame(state, parsedIssued.Value());
    Require(first.Succeeded() && first.advanced
        && duplicate.Succeeded() && !duplicate.advanced
        && state.hasIssuedSequence && state.highestIssuedSequence == 9
        && state.retirePriorTo == 3,
        "first and duplicate NEW_CONNECTION_ID observations should be stable");

    const QuicConnectionIdFrame nextIssued{
        QuicConnectionIdFrameKind::NewConnectionId,
        10,
        4,
        connectionId,
        resetToken,
        0 };
    const auto nextBytes = BuildQuicConnectionIdFrame(nextIssued);
    Require(nextBytes.IsOk(), "second NEW_CONNECTION_ID should build");
    const auto parsedNext = ParseQuicConnectionIdFrame(
        nextBytes.Value().data(), nextBytes.Value().size());
    Require(parsedNext.IsOk(), "second NEW_CONNECTION_ID should parse");
    const auto advanced = ObserveQuicConnectionIdFrame(state, parsedNext.Value());
    Require(advanced.Succeeded() && advanced.advanced
        && state.highestIssuedSequence == 10 && state.retirePriorTo == 4,
        "higher NEW_CONNECTION_ID should advance the lifecycle");

    const auto beforeRegression = state;
    const QuicConnectionIdFrame regressed{
        QuicConnectionIdFrameKind::NewConnectionId,
        9,
        4,
        connectionId,
        resetToken,
        0 };
    const auto regression = ObserveQuicConnectionIdFrame(state, regressed);
    Require(regression.error == QuicConnectionIdObservationError::SequenceRegression
        && state.hasIssuedSequence == beforeRegression.hasIssuedSequence
        && state.highestIssuedSequence == beforeRegression.highestIssuedSequence
        && state.retirePriorTo == beforeRegression.retirePriorTo,
        "regressed sequence must preserve the committed state");

    const QuicConnectionIdFrame retire{
        QuicConnectionIdFrameKind::RetireConnectionId, 9, 0, {}, {}, 0 };
    const auto encodedRetire = BuildQuicConnectionIdFrame(retire);
    Require(encodedRetire.IsOk(), "RETIRE_CONNECTION_ID should build");
    const auto parsedRetire = ParseQuicConnectionIdFrame(
        encodedRetire.Value().data(), encodedRetire.Value().size());
    Require(parsedRetire.IsOk()
        && parsedRetire.Value().kind == QuicConnectionIdFrameKind::RetireConnectionId
        && parsedRetire.Value().sequenceNumber == retire.sequenceNumber
        && parsedRetire.Value().consumedBytes == encodedRetire.Value().size(),
        "RETIRE_CONNECTION_ID should round trip");
    const auto retired = ObserveQuicConnectionIdFrame(state, parsedRetire.Value());
    const auto retiredDuplicate = ObserveQuicConnectionIdFrame(state, parsedRetire.Value());
    Require(retired.Succeeded() && retired.advanced
        && retiredDuplicate.Succeeded() && !retiredDuplicate.advanced
        && state.hasRetiredSequence && state.highestRetiredSequence == 9,
        "RETIRE_CONNECTION_ID duplicate should not advance state");

    const auto beforeUnknownRetire = state;
    const auto unknownRetire = ObserveQuicConnectionIdFrame(state, {
        QuicConnectionIdFrameKind::RetireConnectionId, 11, 0, {}, {}, 0 });
    Require(unknownRetire.error == QuicConnectionIdObservationError::RetireBeyondObserved
        && state.hasRetiredSequence == beforeUnknownRetire.hasRetiredSequence
        && state.highestRetiredSequence == beforeUnknownRetire.highestRetiredSequence,
        "unknown RETIRE_CONNECTION_ID must preserve state");

    const auto beforeMalformed = state;
    std::vector<std::uint8_t> truncated = encodedIssued.Value();
    truncated.pop_back();
    Require(!ParseQuicConnectionIdFrame(truncated.data(), truncated.size()).IsOk(),
        "truncated NEW_CONNECTION_ID must be rejected");
    Require(state.hasIssuedSequence == beforeMalformed.hasIssuedSequence
        && state.highestIssuedSequence == beforeMalformed.highestIssuedSequence
        && state.retirePriorTo == beforeMalformed.retirePriorTo
        && state.highestRetiredSequence == beforeMalformed.highestRetiredSequence,
        "malformed wire input must not mutate lifecycle state");
    Require(!BuildQuicConnectionIdFrame({
        QuicConnectionIdFrameKind::NewConnectionId, 1, 0, {}, resetToken, 0 }).IsOk(),
        "empty connection id must be rejected");

    state.Reset();
    Require(!state.hasIssuedSequence && !state.hasRetiredSequence
        && state.highestIssuedSequence == 0 && state.highestRetiredSequence == 0
        && state.retirePriorTo == 0,
        "Reset should clear connection-id lifecycle state");

    std::cout << "passed=true"
              << " new_roundtrip=true"
              << " retire_roundtrip=true"
              << " token_bytes=true"
              << " monotonic=true"
              << " duplicates=true"
              << " rejection_preserves=true"
              << " reset=true\n";
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
