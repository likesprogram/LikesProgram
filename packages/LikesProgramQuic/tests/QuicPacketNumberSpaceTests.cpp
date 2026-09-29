#include <LikesProgram/Quic/QuicPacketNumberSpace.hpp>

#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicPacketNumberSpaceTests() {
    using namespace LikesProgram::Quic;
    QuicPacketNumberSpace space;
    Require(!space.Snapshot().hasLargestReceived,
        "packet number space should start without an observation");
    Require(space.Decode(0xFE, 1).IsOk(),
        "packet number space should reconstruct the first packet");
    Require(space.Snapshot().hasLargestReceived
        && space.Snapshot().largestReceived == 0xFE,
        "packet number space should retain the first largest packet");
    const auto wrapped = space.Decode(0x02, 1);
    Require(wrapped.IsOk() && wrapped.Value() == 0x102
        && space.Snapshot().largestReceived == 0x102,
        "packet number space should recover a wrapped truncated packet");
    const auto older = space.Decode(0x01, 1);
    Require(older.IsOk() && older.Value() < space.Snapshot().largestReceived
        && space.Snapshot().largestReceived == 0x102,
        "out-of-order packets must not regress the largest observation");
    Require(!space.Decode(0, 5).IsOk()
        && space.Snapshot().largestReceived == 0x102,
        "invalid packet-number width must preserve state");
    Require(space.Observe(0x200).IsOk()
        && space.Snapshot().largestReceived == 0x200,
        "full packet observations should advance the number space");
    Require(space.Observe(0x100).IsOk()
        && space.Snapshot().largestReceived == 0x200,
        "older full observations must not regress state");
    Require(!space.Observe(std::uint64_t{ 1 } << 62).IsOk(),
        "packet number space should reject values outside 62 bits");
    space.Reset();
    Require(!space.Snapshot().hasLargestReceived,
        "packet number space reset should clear observations");
}
