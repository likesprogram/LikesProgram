#include <LikesProgram/Quic/QuicAckApplication.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicCongestionBudget.hpp>
#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    using LikesProgram::Quic::ApplyQuicAckFrame;
    using LikesProgram::Quic::BuildQuicAckFrame;
    using LikesProgram::Quic::ParseQuicAckFrame;
    using LikesProgram::Quic::QuicAckFrame;
    using LikesProgram::Quic::QuicCongestionBudget;
    using LikesProgram::Quic::QuicRetransmissionQueue;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void RequireStatusCode(const auto& result,
        LikesProgram::StatusCode code, const char* message) {
        Require(!result.IsOk() && result.GetStatus().Code() == code, message);
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const auto base = Clock::time_point{};

    QuicRetransmissionQueue queue({ 8, 32 });
    QuicCongestionBudget budget({ 32, 8 });
    const std::vector<std::uint8_t> first{ 1, 2 };
    const std::vector<std::uint8_t> second{ 3, 4, 5 };
    const std::vector<std::uint8_t> third{ 6 };

    Require(queue.Track(10, first, base + std::chrono::seconds(4)).IsOk(),
        "queue should track the first packet");
    Require(queue.Track(12, second, base + std::chrono::seconds(5)).IsOk(),
        "queue should track the second packet");
    Require(queue.Track(13, third, base + std::chrono::seconds(6)).IsOk(),
        "queue should track the third packet");
    RequireStatusCode(queue.Track(12, second, base), LikesProgram::StatusCode::AlreadyExists,
        "duplicate packet numbers should be rejected");
    Require(budget.Reserve(10, first.size()).IsOk()
        && budget.Reserve(12, second.size()).IsOk()
        && budget.Reserve(13, third.size()).IsOk(),
        "budget should reserve every tracked packet");
    Require(queue.Snapshot().trackedPackets == 3
        && queue.Snapshot().trackedBytes == 6
        && budget.Snapshot().inFlightBytes == 6,
        "queue and budget snapshots should agree before ACK");

    QuicAckFrame frame;
    frame.largestAcknowledged = 13;
    frame.ranges = { { 12, 13 }, { 10, 10 } };
    const auto encoded = BuildQuicAckFrame(frame);
    Require(encoded.IsOk() && !encoded.Value().empty(),
        "ACK frame should build from inclusive ranges");
    const auto parsed = ParseQuicAckFrame(encoded.Value().data(), encoded.Value().size());
    Require(parsed.IsOk() && parsed.Value().ranges == frame.ranges
        && parsed.Value().largestAcknowledged == frame.largestAcknowledged,
        "ACK frame should parse back with its ranges");

    const auto applied = ApplyQuicAckFrame(queue, parsed.Value());
    Require(applied.IsOk() && applied.Value().acknowledgedPackets == 3
        && applied.Value().untrackedPackets == 0
        && queue.Snapshot().trackedPackets == 0,
        "ACK application should retire tracked packets in every range");
    Require(budget.Acknowledge(10).IsOk()
        && budget.Acknowledge(12).IsOk()
        && budget.Acknowledge(13).IsOk()
        && budget.Snapshot().inFlightBytes == 0,
        "ACK should release each matching congestion reservation");

    QuicAckFrame untracked;
    untracked.largestAcknowledged = 20;
    untracked.ranges = { { 19, 20 } };
    const auto untrackedResult = ApplyQuicAckFrame(queue, untracked);
    Require(untrackedResult.IsOk() && untrackedResult.Value().acknowledgedPackets == 0
        && untrackedResult.Value().untrackedPackets == 2,
        "ACK application should report untracked packet numbers");

    QuicRetransmissionQueue expiryQueue({ 4, 16 });
    Require(expiryQueue.Track(21, { 7, 8 }, base + std::chrono::seconds(1)).IsOk()
        && expiryQueue.Track(22, { 9 }, base + std::chrono::seconds(3)).IsOk(),
        "expiry queue should accept caller-owned deadlines");
    const auto expired = expiryQueue.CollectExpired(base + std::chrono::seconds(1));
    Require(expired.IsOk() && expired.Value().size() == 1
        && expired.Value()[0].packetNumber == 21
        && expiryQueue.Snapshot().trackedPackets == 1,
        "caller-supplied now should extract only expired packets");

    RequireStatusCode(queue.Track(0, {}, base), LikesProgram::StatusCode::InvalidArgument,
        "empty packet payload should be rejected");
    RequireStatusCode(queue.Track(std::uint64_t{ 1 } << 62, first, base),
        LikesProgram::StatusCode::InvalidArgument,
        "packet numbers beyond QUIC's limit should be rejected");
    QuicRetransmissionQueue limited({ 1, 2 });
    Require(limited.Track(30, first, base).IsOk(),
        "limited queue should accept its first packet");
    RequireStatusCode(limited.Track(31, third, base), LikesProgram::StatusCode::ResourceExhausted,
        "queue packet limits should be enforced");

    QuicCongestionBudget edgeBudget({ 8, 2 });
    RequireStatusCode(edgeBudget.Reserve(0, 1), LikesProgram::StatusCode::InvalidArgument,
        "zero packet reservations should be rejected");
    RequireStatusCode(edgeBudget.Reserve(1, 9), LikesProgram::StatusCode::ResourceExhausted,
        "reservations above the available window should be rejected");
    Require(edgeBudget.Reserve(1, 5).IsOk(),
        "edge budget should reserve in-window bytes");
    RequireStatusCode(edgeBudget.Reserve(1, 1), LikesProgram::StatusCode::AlreadyExists,
        "duplicate congestion reservations should be rejected");
    RequireStatusCode(edgeBudget.SetWindow(4), LikesProgram::StatusCode::ResourceExhausted,
        "window cannot shrink below in-flight bytes");
    Require(edgeBudget.Lose(1).IsOk() && edgeBudget.Snapshot().inFlightBytes == 0,
        "loss should release a reservation");
    RequireStatusCode(edgeBudget.Lose(1), LikesProgram::StatusCode::NotFound,
        "duplicate loss should be rejected");

    QuicRetransmissionQueue movedQueue({ 2, 8 });
    Require(movedQueue.Track(40, { 1, 2 }, base).IsOk(),
        "move source should track a packet");
    QuicRetransmissionQueue movedTo(std::move(movedQueue));
    Require(movedTo.Snapshot().trackedPackets == 1
        && movedQueue.Snapshot().trackedPackets == 0,
        "move should transfer queue bookkeeping");
    movedTo.Reset();
    Require(movedTo.Snapshot().trackedPackets == 0
        && movedTo.Snapshot().trackedBytes == 0,
        "queue reset should clear packet bookkeeping");
    edgeBudget.Reset();
    Require(edgeBudget.Snapshot().availableBytes == 8
        && edgeBudget.Snapshot().reservations == 0,
        "budget reset should restore its configured window");

    std::cout << "passed=true"
              << " acked=3"
              << " untracked=2"
              << " expired=1"
              << " congestion_released=true"
              << " limits_rejected=true"
              << " move_reset=true\n";
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
