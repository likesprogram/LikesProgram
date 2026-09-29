#include <LikesProgram/Quic/QuicDatagramFrame.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicDatagramFrameTests() {
    using namespace LikesProgram::Quic;
    const std::vector<std::uint8_t> payload{ 0, 1, 2, 3 };
    for (const bool hasLength : { false, true }) {
        const auto encoded = BuildQuicDatagramFrame(hasLength, payload);
        Require(encoded.IsOk());
        auto input = encoded.Value();
        if (hasLength) input.push_back(0xA5);
        const auto parsed = ParseQuicDatagramFrame(input.data(), input.size());
        Require(parsed.IsOk() && parsed.Value().hasLength == hasLength
            && parsed.Value().data.size() == payload.size()
            && parsed.Value().consumedBytes
                == (hasLength ? encoded.Value().size() : input.size()));
        Require(std::equal(parsed.Value().data.begin(), parsed.Value().data.end(),
            payload.begin()));
    }
    const std::uint8_t truncated[] = { 0x31, 0x04, 0x01 };
    Require(!ParseQuicDatagramFrame(truncated, sizeof(truncated)).IsOk());
    const std::uint8_t wrong[] = { 0x01 };
    Require(!ParseQuicDatagramFrame(wrong, sizeof(wrong)).IsOk());
    Require(BuildQuicDatagramFrame(true, {}).IsOk());
}
