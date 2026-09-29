#include <LikesProgram/Quic/QuicCryptoFrame.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicCryptoFrameTests() {
    using namespace LikesProgram::Quic;

    const std::array<std::uint8_t, 4> payload{ 0x01, 0x02, 0x03, 0x04 };
    QuicCryptoFrameBuildOptions options;
    options.offset = 64;
    options.data = payload;
    const auto encoded = BuildQuicCryptoFrame(options);
    Require(encoded.IsOk());
    Require(encoded.Value().front() == 0x06);
    const std::array<std::uint8_t, 1> trailing{ 0xFF };
    std::vector<std::uint8_t> withTrailing = encoded.Value();
    // 单字节追加尾随数据，避免 GCC 15 对范围插入的数组边界误报。
    withTrailing.push_back(trailing.front());
    const auto decoded = ParseQuicCryptoFrame(withTrailing.data(), withTrailing.size());
    Require(decoded.IsOk());
    Require(decoded.Value().offset == 64);
    Require(decoded.Value().consumedBytes == encoded.Value().size());
    Require(std::vector<std::uint8_t>(
        decoded.Value().data.begin(), decoded.Value().data.end())
        == std::vector<std::uint8_t>(payload.begin(), payload.end()));

    const std::array<std::uint8_t, 7> nonMinimalType{
        0x40, 0x06, 0x40, 0x25, 0x01, 0x01, 0xAA };
    const auto nonMinimalDecoded = ParseQuicCryptoFrame(
        nonMinimalType.data(), nonMinimalType.size());
    Require(nonMinimalDecoded.IsOk());
    Require(nonMinimalDecoded.Value().offset == 37);
    Require(nonMinimalDecoded.Value().data.size() == 1);

    Require(!ParseQuicCryptoFrame(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> wrongType{ 0x02 };
    Require(!ParseQuicCryptoFrame(wrongType.data(), wrongType.size()).IsOk());
    const std::array<std::uint8_t, 2> truncated{ 0x06, 0x40 };
    Require(!ParseQuicCryptoFrame(truncated.data(), truncated.size()).IsOk());
    const std::array<std::uint8_t, 4> tooLong{ 0x06, 0x00, 0x05, 0xAA };
    Require(!ParseQuicCryptoFrame(tooLong.data(), tooLong.size()).IsOk());
}
