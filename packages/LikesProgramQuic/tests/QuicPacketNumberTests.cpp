#include <LikesProgram/Quic/QuicPacketNumber.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    std::vector<std::uint8_t> Copy(
        const LikesProgram::Quic::QuicPacketNumberEncoding& encoding) {
        return { encoding.storage.begin(), encoding.storage.begin() + encoding.size };
    }
}

void RunQuicPacketNumberTests() {
    using namespace LikesProgram::Quic;

    auto encoded = EncodeQuicPacketNumber(0);
    Require(encoded.IsOk());
    Require(encoded.Value().size == 1);
    Require(Copy(encoded.Value()) == std::vector<std::uint8_t>{ 0 });

    encoded = EncodeQuicPacketNumber(0xAC5C02, 0xABE8B3);
    Require(encoded.IsOk());
    Require(encoded.Value().size == 2);
    Require(Copy(encoded.Value()) == std::vector<std::uint8_t>{ 0x5C, 0x02 });

    encoded = EncodeQuicPacketNumber(0xACE8FE, 0xABE8B3);
    Require(encoded.IsOk());
    Require(encoded.Value().size == 3);
    Require(Copy(encoded.Value())
        == std::vector<std::uint8_t>{ 0xAC, 0xE8, 0xFE });

    Require(DecodeQuicPacketNumber(0xA82F30EA, 0x9B32, 2).Value()
        == 0xA82F9B32);
    Require(DecodeQuicPacketNumber(0xFFF0, 0x0005, 2).Value()
        == 0x10005);
    Require(DecodeQuicPacketNumber(0x10005, 0xFFF0, 2).Value()
        == 0xFFF0);
    Require(DecodeQuicPacketNumber(100, 101, 1).Value() == 101);

    Require(!EncodeQuicPacketNumber(1, 2).IsOk());
    Require(!EncodeQuicPacketNumber(
        (std::uint64_t{ 1 } << 62)).IsOk());
    Require(!EncodeQuicPacketNumber(
        (std::uint64_t{ 1 } << 32), std::nullopt).IsOk());
    Require(!DecodeQuicPacketNumber(0, 0, 0).IsOk());
    Require(!DecodeQuicPacketNumber(0, 0, 5).IsOk());
    Require(!DecodeQuicPacketNumber(0, 256, 1).IsOk());
    Require(!DecodeQuicPacketNumber(
        (std::uint64_t{ 1 } << 62) - 1, 0, 1).IsOk());
}
