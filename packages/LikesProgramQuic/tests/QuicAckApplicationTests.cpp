#include <LikesProgram/Quic/QuicAckApplication.hpp>

#include <chrono>
#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicAckApplicationTests() {
    using namespace LikesProgram::Quic;
    using Clock = std::chrono::steady_clock;
    const auto base = Clock::time_point{};

    QuicRetransmissionQueue queue({ 8, 32 });
    Require(queue.Track(10, { 1 }, base).IsOk(),
        "ACK application should start with a tracked packet");
    Require(queue.Track(12, { 2 }, base).IsOk(),
        "ACK application should track a second packet");
    Require(queue.Track(13, { 3 }, base).IsOk(),
        "ACK application should track a third packet");
    QuicAckFrame frame;
    frame.largestAcknowledged = 13;
    frame.ranges = { { 12, 13 }, { 10, 10 } };
    const auto applied = ApplyQuicAckFrame(queue, frame);
    Require(applied.IsOk() && applied.Value().acknowledgedPackets == 3
        && applied.Value().untrackedPackets == 0
        && queue.Snapshot().trackedPackets == 0,
        "ACK application should retire tracked packets across ordered ranges");

    QuicAckFrame withGap;
    withGap.largestAcknowledged = 20;
    withGap.ranges = { { 10, 20 } };
    const auto ignored = ApplyQuicAckFrame(queue, withGap);
    Require(ignored.IsOk() && ignored.Value().acknowledgedPackets == 0
        && ignored.Value().untrackedPackets == 11,
        "ACK application should count untracked packet numbers without failure");

    QuicAckFrame overlap;
    overlap.largestAcknowledged = 13;
    overlap.ranges = { { 10, 13 }, { 9, 10 } };
    Require(!ApplyQuicAckFrame(queue, overlap).IsOk(),
        "ACK application should reject overlapping ranges before mutation");

    QuicAckFrame oversized;
    oversized.largestAcknowledged = 20;
    oversized.ranges = { { 0, 20 } };
    Require(!ApplyQuicAckFrame(queue, oversized, { 8 }).IsOk(),
        "ACK application should reject coverage beyond its explicit bound");
}
