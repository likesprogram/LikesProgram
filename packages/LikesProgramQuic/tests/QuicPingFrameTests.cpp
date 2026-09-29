#include <LikesProgram/Quic/QuicPingFrame.hpp>

#include <cstdint>
#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicPingFrameTests() {
    using namespace LikesProgram::Quic;
    const auto encoded = BuildQuicPingFrame();
    Require(encoded.IsOk() && encoded.Value() == std::vector<std::uint8_t>{ 0x01 });
    const std::uint8_t input[] = { 0x01, 0xA5 };
    const auto parsed = ParseQuicPingFrame(input, sizeof(input));
    Require(parsed.IsOk() && parsed.Value().consumedBytes == 1);
    const std::uint8_t wrong[] = { 0x00 };
    Require(!ParseQuicPingFrame(wrong, sizeof(wrong)).IsOk());
    Require(!ParseQuicPingFrame(nullptr, 0).IsOk());
}
