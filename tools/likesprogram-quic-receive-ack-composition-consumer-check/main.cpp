#include <LikesProgram/Quic/QuicAckApplication.hpp>
#include <LikesProgram/Quic/QuicAckDelay.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicAckTracker.hpp>
#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>
#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const auto base = Clock::time_point{};

    QuicPacketNumberSpace numberSpace;
    const auto first = numberSpace.Decode(0xfe, 1);
    const auto second = numberSpace.Decode(0x02, 1);
    const auto third = numberSpace.Decode(0x03, 1);
    Require(first.IsOk() && first.Value() == 0xfe
        && second.IsOk() && second.Value() == 0x102
        && third.IsOk() && third.Value() == 0x103
        && numberSpace.Snapshot().largestReceived == 0x103,
        "packet-number reconstruction should feed a monotonic receive space");

    QuicAckDelayContext delayContext;
    Require(ConfigureQuicAckDelay(delayContext, 3, 80)
        == QuicAckDelayError::None,
        "caller-supplied ACK delay context should configure");

    QuicAckObservationState observation;
    QuicAckFrame ack;
    ack.ecn = true;
    ack.largestAcknowledged = 0x103;
    ack.ackDelay = 10;
    ack.ranges = { { 0x101, 0x103 }, { 0xfe, 0xfe } };
    ack.ect0Count = 4;
    ack.ect1Count = 2;
    ack.ecnCeCount = 1;
    const auto encoded = BuildQuicAckFrame(ack);
    Require(encoded.IsOk() && !encoded.Value().empty(),
        "ACK composition should build the observed frame");
    const auto parsed = ParseQuicAckFrame(encoded.Value().data(), encoded.Value().size());
    Require(parsed.IsOk() && parsed.Value().ranges == ack.ranges
        && parsed.Value().ecn && parsed.Value().ackDelay == ack.ackDelay,
        "ACK composition should parse back its ranges, ECN and delay");

    const auto observed = ObserveQuicAck(observation, parsed.Value(), delayContext);
    Require(observed.Succeeded() && observed.advancesLargest
        && observation.largestObserved == 0x103
        && observation.observedFrames == 1
        && observation.ect0Observed == 4
        && observation.ect1Observed == 2
        && observation.ecnCeObserved == 1,
        "valid ACK delay and ECN values should advance observation state");

    QuicAckFrame older = ack;
    older.ecn = false;
    older.largestAcknowledged = 0x102;
    older.ranges = { { 0x102, 0x102 } };
    const auto delayed = ObserveQuicAck(observation, older, delayContext);
    Require(delayed.Succeeded() && !delayed.advancesLargest
        && observation.largestObserved == 0x103
        && observation.observedFrames == 2,
        "an older delayed ACK should be accepted without regressing largest state");

    const auto framesBeforeInvalid = observation.observedFrames;
    QuicAckFrame invalidDelay = ack;
    invalidDelay.ackDelay = 11;
    Require(ObserveQuicAck(observation, invalidDelay, delayContext).error
        == QuicAckObservationError::AckDelayInvalid
        && observation.observedFrames == framesBeforeInvalid,
        "invalid ACK delay should leave observation state unchanged");
    QuicAckFrame invalidOrder = ack;
    invalidOrder.ranges = { { 0x101, 0x103 }, { 0x100, 0x101 } };
    Require(ObserveQuicAck(observation, invalidOrder).error
        == QuicAckObservationError::InvalidOrder
        && observation.observedFrames == framesBeforeInvalid,
        "overlapping ACK ranges should leave observation state unchanged");
    QuicAckFrame invalidEcn = ack;
    invalidEcn.ect1Count = 1;
    Require(ObserveQuicAck(observation, invalidEcn).error
        == QuicAckObservationError::EcnCounterRegression
        && observation.ect1Observed == 2
        && observation.observedFrames == framesBeforeInvalid,
        "regressed ECN counters should leave observation state unchanged");

    QuicRetransmissionQueue queue({ 8, 32 });
    Require(queue.Track(0xfe, { 1 }, base + std::chrono::seconds(1)).IsOk()
        && queue.Track(0x101, { 2, 3 }, base + std::chrono::seconds(1)).IsOk()
        && queue.Track(0x102, { 4 }, base + std::chrono::seconds(1)).IsOk(),
        "receive ACK composition should start with tracked sent packets");
    const auto applied = ApplyQuicAckFrame(queue, parsed.Value());
    Require(applied.IsOk() && applied.Value().acknowledgedPackets == 3
        && applied.Value().untrackedPackets == 1
        && queue.Snapshot().trackedPackets == 0,
        "validated ACK ranges should retire tracked packets and report a gap");

    numberSpace.Reset();
    delayContext.Reset();
    observation.Reset();
    Require(!numberSpace.Snapshot().hasLargestReceived
        && !delayContext.configured
        && observation.observedFrames == 0,
        "receive and ACK observation state should reset independently");

    std::cout << "passed=true"
              << " decoded=3"
              << " acked=3"
              << " untracked=1"
              << " delayed_nonregress=true"
              << " invalid_preserves=true"
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
