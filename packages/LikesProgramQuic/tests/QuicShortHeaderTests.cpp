#include <LikesProgram/Quic/QuicShortHeader.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicShortHeaderTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 3> connectionId{ 1, 2, 3 };
    const std::array<std::uint8_t, 2> packetNumber{ 0x12, 0x34 };
    const std::array<std::uint8_t, 3> payload{ 9, 8, 7 };
    auto built = BuildQuicShortHeaderPacket({ true, true, connectionId,
        packetNumber, payload });
    Require(built.IsOk(), "short header should build");
    auto parsed = ParseQuicShortHeader(built.Value().data(), built.Value().size(),
        connectionId.size());
    Require(parsed.IsOk() && parsed.Value().spinBit && parsed.Value().keyPhase
        && parsed.Value().packetNumberLength == 2
        && parsed.Value().destinationConnectionId.size() == connectionId.size()
        && parsed.Value().packetNumber[0] == 0x12
        && parsed.Value().payload.size() == payload.size(),
        "short header should round-trip fields");

    auto reserved = built.Value();
    reserved[0] = static_cast<std::uint8_t>(reserved[0] | 0x08);
    Require(!ParseQuicShortHeader(reserved.data(), reserved.size(),
        connectionId.size()).IsOk(), "reserved short-header bits should fail");
    Require(!ParseQuicShortHeader(built.Value().data(), 2,
        connectionId.size()).IsOk(), "truncated short header should fail");
    Require(!BuildQuicShortHeaderPacket({ false, false, {}, {}, payload }).IsOk(),
        "short header should require a packet number");
}
