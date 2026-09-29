#include <LikesProgram/Quic/QuicConnectionCredit.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamCredit.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    QuicFlowControlFrame ParseWithTrailing(
        const QuicFlowControlFrame& frame,
        bool& consumedBoundary) {
        const auto encoded = BuildQuicFlowControlFrame(frame);
        Require(encoded.IsOk(), "flow-control frame should build");
        std::vector<std::uint8_t> input = encoded.Value();
        input.push_back(0xA5);
        const auto parsed = ParseQuicFlowControlFrame(input.data(), input.size());
        Require(parsed.IsOk(), "flow-control frame should parse");
        consumedBoundary = parsed.Value().consumedBytes == encoded.Value().size();
        return parsed.Value();
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    bool connectionBoundary = false;
    const auto parsedConnection = ParseWithTrailing(
        { QuicFlowControlFrameKind::MaxData, 0, 1024, 0 }, connectionBoundary);
    Require(connectionBoundary
        && parsedConnection.kind == QuicFlowControlFrameKind::MaxData
        && parsedConnection.streamId == 0
        && parsedConnection.limit == 1024,
        "MAX_DATA should preserve connection coordinates and consumed boundary");

    QuicConnectionCreditState connection;
    const auto connectionAdvance = ApplyQuicPeerConnectionCredit(
        connection, parsedConnection.limit);
    const auto connectionDuplicate = ApplyQuicPeerConnectionCredit(
        connection, parsedConnection.limit);
    Require(connectionAdvance.Succeeded() && connectionAdvance.previous == 0
        && connectionAdvance.current == 1024
        && connectionDuplicate.Succeeded() && connectionDuplicate.previous == 1024
        && connection.limit == 1024 && connection.Available() == 1024,
        "MAX_DATA should advance and accept an idempotent duplicate");

    bool streamBoundary = false;
    const auto parsedStream = ParseWithTrailing(
        { QuicFlowControlFrameKind::MaxStreamData, 8, 512, 0 }, streamBoundary);
    Require(streamBoundary
        && parsedStream.kind == QuicFlowControlFrameKind::MaxStreamData
        && parsedStream.streamId == 8
        && parsedStream.limit == 512,
        "MAX_STREAM_DATA should preserve stream coordinates and boundary");

    QuicStreamCreditState stream;
    const auto streamAdvance = ApplyQuicPeerStreamCredit(
        stream, parsedStream.limit);
    Require(streamAdvance.Succeeded() && stream.limit == 512
        && stream.Available() == 512,
        "MAX_STREAM_DATA should advance the selected stream ledger");

    const auto nextStream = BuildQuicFlowControlFrame({
        QuicFlowControlFrameKind::MaxStreamData, 8, 768, 0 });
    Require(nextStream.IsOk(), "higher MAX_STREAM_DATA should build");
    const auto parsedNextStream = ParseQuicFlowControlFrame(
        nextStream.Value().data(), nextStream.Value().size());
    Require(parsedNextStream.IsOk(), "higher MAX_STREAM_DATA should parse");
    const auto streamIncrease = ApplyQuicPeerStreamCredit(
        stream, parsedNextStream.Value().limit);
    Require(streamIncrease.Succeeded() && stream.limit == 768,
        "higher stream limit should advance monotonically");

    const auto beforeRegression = stream;
    const auto regression = ApplyQuicPeerStreamCredit(stream, 511);
    Require(regression.error == QuicStreamCreditError::LimitRegression
        && stream.limit == beforeRegression.limit
        && stream.reserved == beforeRegression.reserved,
        "stream limit regression must preserve state");

    const auto beforeBlocked = connection;
    const auto blocked = BuildQuicFlowControlFrame({
        QuicFlowControlFrameKind::DataBlocked, 0, 1024, 0 });
    Require(blocked.IsOk(), "DATA_BLOCKED should build");
    const auto parsedBlocked = ParseQuicFlowControlFrame(
        blocked.Value().data(), blocked.Value().size());
    Require(parsedBlocked.IsOk()
        && parsedBlocked.Value().kind == QuicFlowControlFrameKind::DataBlocked,
        "DATA_BLOCKED should remain an observable non-credit frame");
    Require(connection.limit == beforeBlocked.limit
        && connection.reserved == beforeBlocked.reserved,
        "blocked frame must not mutate connection credit");

    const auto beforeInvalid = connection;
    connection.reserved = connection.limit + 1;
    const auto invalidState = ApplyQuicPeerConnectionCredit(connection, 2048);
    Require(invalidState.error == QuicConnectionCreditError::InvalidState
        && connection.limit == beforeInvalid.limit
        && connection.reserved == beforeInvalid.limit + 1,
        "invalid credit state must be reported before mutation");
    connection = beforeInvalid;

    const std::uint8_t wrongType[] = { 0x08, 0x00, 0x00 };
    const std::uint8_t truncated[] = { 0x11, 0x40 };
    Require(!ParseQuicFlowControlFrame(wrongType, sizeof(wrongType)).IsOk()
        && !ParseQuicFlowControlFrame(truncated, sizeof(truncated)).IsOk(),
        "wrong type and truncated flow-control input must be rejected");

    connection.Reset();
    stream.Reset();
    Require(connection.limit == 0 && connection.reserved == 0
        && stream.limit == 0 && stream.reserved == 0,
        "Reset should clear both independent credit ledgers");

    std::cout << "passed=true"
              << " max_data=true"
              << " max_stream_data=true"
              << " consumed_boundary=true"
              << " monotonic=true"
              << " duplicate=true"
              << " regression_preserves=true"
              << " blocked_nonmutation=true"
              << " invalid_state=true"
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
