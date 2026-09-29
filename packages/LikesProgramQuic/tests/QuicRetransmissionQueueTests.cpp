#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>

#include <array>
#include <chrono>
#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicRetransmissionQueueTests() {
    using namespace LikesProgram::Quic;
    using Clock = std::chrono::steady_clock;
    const auto base = Clock::time_point{};
    QuicRetransmissionQueue queue({ 2, 4 });
    const std::vector<std::uint8_t> first{ 1, 2 };
    const std::vector<std::uint8_t> second{ 3 };
    Require(queue.Track(7, first, base + std::chrono::seconds(2)).IsOk(),
        "retransmission queue should track packet");
    Require(queue.Track(9, second, base + std::chrono::seconds(1)).IsOk(),
        "retransmission queue should track a second packet");
    Require(queue.Snapshot().trackedPackets == 2
        && queue.Snapshot().trackedBytes == 3,
        "retransmission queue should expose accounting");
    Require(!queue.Track(7, first, base + std::chrono::seconds(3)).IsOk(),
        "retransmission queue should reject duplicate packet numbers");
    auto expired = queue.CollectExpired(base + std::chrono::seconds(1));
    Require(expired.IsOk() && expired.Value().size() == 1
        && expired.Value()[0].packetNumber == 9,
        "retransmission queue should extract expired packets in order");
    Require(queue.Acknowledge(7).IsOk() && queue.Snapshot().trackedPackets == 0,
        "acknowledgement should retire a tracked packet");
    Require(!queue.Acknowledge(7).IsOk(),
        "acknowledgement should reject an unknown packet");
    Require(!queue.Track(11, { 1, 2, 3, 4, 5 }, base).IsOk(),
        "retransmission queue should enforce byte limits");
}
