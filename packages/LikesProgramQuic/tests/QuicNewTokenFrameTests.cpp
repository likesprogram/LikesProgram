#include <LikesProgram/Quic/QuicNewTokenFrame.hpp>

#include <array>
#include <cstdlib>
#include <span>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicNewTokenFrameTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 3> token{ 0xA1, 0xB2, 0xC3 };
    const auto encoded = BuildQuicNewTokenFrame({ token });
    Require(encoded.IsOk());
    Require(encoded.Value().size() == 5);
    Require(encoded.Value()[0] == 0x07);
    Require(encoded.Value()[1] == 3);
    const auto parsed = ParseQuicNewTokenFrame(
        encoded.Value().data(), encoded.Value().size());
    Require(parsed.IsOk());
    Require(parsed.Value().consumedBytes == encoded.Value().size());
    Require(parsed.Value().token.size() == token.size());
    Require(parsed.Value().token[1] == 0xB2);

    const std::array<std::uint8_t, 2> emptyTokenFrame{ 0x07, 0x00 };
    const auto empty = ParseQuicNewTokenFrame(
        emptyTokenFrame.data(), emptyTokenFrame.size());
    Require(empty.IsOk());
    Require(empty.Value().token.empty());

    const std::array<std::uint8_t, 4> nonMinimalLength{ 0x07, 0x40, 0x01, 0xA5 };
    const auto nonMinimal = ParseQuicNewTokenFrame(
        nonMinimalLength.data(), nonMinimalLength.size());
    Require(nonMinimal.IsOk());
    Require(nonMinimal.Value().token.size() == 1);

    const std::array<std::uint8_t, 2> wrongType{ 0x06, 0x00 };
    Require(!ParseQuicNewTokenFrame(wrongType.data(), wrongType.size()).IsOk());
    const std::array<std::uint8_t, 2> truncated{ 0x07, 0x02 };
    Require(!ParseQuicNewTokenFrame(truncated.data(), truncated.size()).IsOk());
    Require(!ParseQuicNewTokenFrame(nullptr, 0).IsOk());
}
