#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicHandshakeDoneFrameTests() {
    using namespace LikesProgram::Quic;
    const auto encoded = BuildQuicHandshakeDoneFrame();
    Require(encoded.IsOk() && encoded.Value() == std::vector<std::uint8_t>{ 0x1E });
    const std::uint8_t input[] = { 0x1E, 0xA5 };
    const auto parsed = ParseQuicHandshakeDoneFrame(input, sizeof(input));
    Require(parsed.IsOk() && parsed.Value().consumedBytes == 1);
    const std::uint8_t wrong[] = { 0x01 };
    Require(!ParseQuicHandshakeDoneFrame(wrong, sizeof(wrong)).IsOk());
    Require(!ParseQuicHandshakeDoneFrame(nullptr, 0).IsOk());
}
