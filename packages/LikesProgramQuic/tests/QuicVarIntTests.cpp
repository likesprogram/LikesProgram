#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    template <std::size_t Size>
    void RequireDecodes(
        const std::array<std::uint8_t, Size>& bytes,
        std::uint64_t expectedValue,
        std::size_t expectedBytes = Size) {
        const auto decoded = LikesProgram::Quic::ParseQuicVarInt(
            bytes.data(), bytes.size());
        Require(decoded.IsOk());
        Require(decoded.Value().value == expectedValue);
        Require(decoded.Value().encodedBytes == expectedBytes);
    }

    template <std::size_t Size>
    void RequireEncoding(
        std::uint64_t value,
        LikesProgram::Quic::QuicVarIntWidth width,
        const std::array<std::uint8_t, Size>& expected) {
        const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(value, width);
        Require(encoded.IsOk());
        Require(encoded.Value().size == Size);
        for (std::size_t index = 0; index < Size; ++index) {
            Require(encoded.Value().storage[index] == expected[index]);
        }
    }

    void RequireRoundTrip(std::uint64_t value) {
        const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(value);
        Require(encoded.IsOk());
        const auto decoded = LikesProgram::Quic::ParseQuicVarInt(
            encoded.Value().Data(), encoded.Value().size);
        Require(decoded.IsOk());
        Require(decoded.Value().value == value);
        Require(decoded.Value().encodedBytes == encoded.Value().size);
    }
}

void RunQuicVarIntTests() {
    using namespace LikesProgram::Quic;

    RequireDecodes(std::array<std::uint8_t, 1>{ 0x25 }, 37);
    RequireDecodes(std::array<std::uint8_t, 2>{ 0x7B, 0xBD }, 15'293);
    RequireDecodes(
        std::array<std::uint8_t, 4>{ 0x9D, 0x7F, 0x3E, 0x7D },
        494'878'333);
    RequireDecodes(
        std::array<std::uint8_t, 8>{
            0xC2, 0x19, 0x7C, 0x5E, 0xFF, 0x14, 0xE8, 0x8C },
        151'288'809'941'952'652ULL);
    RequireDecodes(std::array<std::uint8_t, 2>{ 0x40, 0x25 }, 37);

    const std::array<std::uint8_t, 3> trailing{ 0x25, 0xAA, 0xBB };
    RequireDecodes(trailing, 37, 1);

    Require(QuicVarIntEncodedSize(0) == 1);
    Require(QuicVarIntEncodedSize(63) == 1);
    Require(QuicVarIntEncodedSize(64) == 2);
    Require(QuicVarIntEncodedSize(16'383) == 2);
    Require(QuicVarIntEncodedSize(16'384) == 4);
    Require(QuicVarIntEncodedSize(1'073'741'823) == 4);
    Require(QuicVarIntEncodedSize(1'073'741'824) == 8);
    Require(QuicVarIntEncodedSize(kQuicVarIntMaximum) == 8);
    Require(QuicVarIntEncodedSize(kQuicVarIntMaximum + 1) == 0);

    for (const std::uint64_t value : std::array<std::uint64_t, 9>{
        0, 63, 64, 16'383, 16'384, 1'073'741'823,
        1'073'741'824, kQuicVarIntMaximum - 1, kQuicVarIntMaximum }) {
        RequireRoundTrip(value);
    }

    RequireEncoding(37, QuicVarIntWidth::Minimum,
        std::array<std::uint8_t, 1>{ 0x25 });
    RequireEncoding(37, QuicVarIntWidth::Two,
        std::array<std::uint8_t, 2>{ 0x40, 0x25 });
    RequireEncoding(15'293, QuicVarIntWidth::Minimum,
        std::array<std::uint8_t, 2>{ 0x7B, 0xBD });
    RequireEncoding(494'878'333, QuicVarIntWidth::Minimum,
        std::array<std::uint8_t, 4>{ 0x9D, 0x7F, 0x3E, 0x7D });
    RequireEncoding(151'288'809'941'952'652ULL, QuicVarIntWidth::Minimum,
        std::array<std::uint8_t, 8>{
            0xC2, 0x19, 0x7C, 0x5E, 0xFF, 0x14, 0xE8, 0x8C });

    Require(!ParseQuicVarInt(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> incompleteTwo{ 0x40 };
    Require(!ParseQuicVarInt(
        incompleteTwo.data(), incompleteTwo.size()).IsOk());
    const std::array<std::uint8_t, 3> incompleteFour{ 0x80, 0x00, 0x00 };
    Require(!ParseQuicVarInt(
        incompleteFour.data(), incompleteFour.size()).IsOk());
    const std::array<std::uint8_t, 7> incompleteEight{
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    Require(!ParseQuicVarInt(
        incompleteEight.data(), incompleteEight.size()).IsOk());
    Require(!EncodeQuicVarInt(kQuicVarIntMaximum + 1).IsOk());
    Require(!EncodeQuicVarInt(64, QuicVarIntWidth::One).IsOk());
    Require(!EncodeQuicVarInt(
        0, static_cast<QuicVarIntWidth>(3)).IsOk());
}
