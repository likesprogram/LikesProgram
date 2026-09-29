#include <LikesProgram/Quic/QuicAckRecoveryLedger.hpp>

#include <chrono>
#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicAckRecoveryLedgerTests() {
    using namespace LikesProgram::Quic;
    using Clock = std::chrono::steady_clock;
    const auto base = Clock::time_point{};
    QuicAckRecoveryLedger ledger({ { 8, 32 }, { 64 } });

    Require(ledger.TrackSent(10, { 1, 2 }, base + std::chrono::seconds(2)).IsOk(),
        "ACK recovery ledger should track sent packet");
    Require(ledger.TrackSent(12, { 3 }, base + std::chrono::seconds(3)).IsOk(),
        "ACK recovery ledger should track a second packet");

    QuicAckFrame ack;
    ack.largestAcknowledged = 12;
    ack.ranges = { { 12, 12 }, { 10, 10 } };
    const auto applied = ledger.ApplyAck(ack);
    Require(applied.IsOk()
        && applied.Value().observation.advancesLargest
        && applied.Value().application.acknowledgedPackets == 2
        && applied.Value().snapshot.retransmission.trackedPackets == 0,
        "ACK recovery ledger should atomically observe and retire packets");

    Require(ledger.TrackSent(20, { 9 }, base + std::chrono::seconds(4)).IsOk(),
        "ACK recovery ledger should track packet for invalid ACK rollback");
    const auto before = ledger.Snapshot();
    QuicAckFrame invalid;
    invalid.largestAcknowledged = 20;
    invalid.ranges = { { 20, 20 }, { 19, 20 } };
    Require(!ledger.ApplyAck(invalid).IsOk(),
        "ACK recovery ledger should reject overlapping ACK before mutation");
    const auto after = ledger.Snapshot();
    Require(after.acknowledgement.largestObserved == before.acknowledgement.largestObserved
        && after.acknowledgement.observedFrames == before.acknowledgement.observedFrames
        && after.retransmission.trackedPackets == before.retransmission.trackedPackets,
        "invalid ACK should preserve observation and retransmission state");

    const auto expired = ledger.CollectExpired(base + std::chrono::seconds(4));
    Require(expired.IsOk() && expired.Value().size() == 1
        && expired.Value()[0].packetNumber == 20
        && ledger.Snapshot().retransmission.trackedPackets == 0,
        "caller-supplied recovery time should extract expired packet");

    QuicAckDelayContext delay;
    Require(ConfigureQuicAckDelay(delay, 3, 80) == QuicAckDelayError::None,
        "ACK recovery ledger should configure delay context");
    QuicAckFrame delayed;
    delayed.largestAcknowledged = 0;
    delayed.ackDelay = 0;
    delayed.ranges = { { 0, 0 } };
    Require(ledger.ApplyAck(delayed, delay).IsOk(),
        "ACK recovery ledger should accept configured ACK delay");
    ledger.Reset();
    Require(ledger.Snapshot().acknowledgement.observedFrames == 0
        && ledger.Snapshot().retransmission.trackedPackets == 0,
        "ACK recovery ledger reset should clear both domains");
}
