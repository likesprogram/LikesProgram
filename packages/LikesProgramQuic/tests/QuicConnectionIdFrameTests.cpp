#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicConnectionIdFrameTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 4> connectionId{ 1, 2, 3, 4 };
    const std::array<std::uint8_t, 16> token{ 0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15 };
    const QuicConnectionIdFrame frame{
        QuicConnectionIdFrameKind::NewConnectionId, 9, 3, connectionId, token, 0 };
    const auto encoded = BuildQuicConnectionIdFrame(frame);
    Require(encoded.IsOk());
    std::vector<std::uint8_t> input = encoded.Value();
    input.push_back(0xA5);
    const auto parsed = ParseQuicConnectionIdFrame(input.data(), input.size());
    Require(parsed.IsOk());
    Require(parsed.Value().kind == QuicConnectionIdFrameKind::NewConnectionId);
    Require(parsed.Value().sequenceNumber == 9 && parsed.Value().retirePriorTo == 3);
    Require(parsed.Value().connectionId.size() == connectionId.size());
    Require(parsed.Value().statelessResetToken.size() == token.size());
    Require(parsed.Value().consumedBytes == encoded.Value().size());

    const auto retire = BuildQuicConnectionIdFrame({
        QuicConnectionIdFrameKind::RetireConnectionId, 4, 0, {}, {}, 0 });
    Require(retire.IsOk());
    const auto retireParsed = ParseQuicConnectionIdFrame(
        retire.Value().data(), retire.Value().size());
    Require(retireParsed.IsOk());
    Require(retireParsed.Value().kind == QuicConnectionIdFrameKind::RetireConnectionId);
    Require(retireParsed.Value().sequenceNumber == 4);

    const std::uint8_t nonMinimal[] = { 0x40, 0x18, 0x02, 0x01, 0x01, 0xAA,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    Require(ParseQuicConnectionIdFrame(nonMinimal, sizeof(nonMinimal)).IsOk());
    const std::uint8_t wrongType[] = { 0x10, 0 };
    Require(!ParseQuicConnectionIdFrame(wrongType, sizeof(wrongType)).IsOk());
    const std::uint8_t badLength[] = { 0x18, 0, 0, 0 };
    Require(!ParseQuicConnectionIdFrame(badLength, sizeof(badLength)).IsOk());
    Require(!BuildQuicConnectionIdFrame({
        QuicConnectionIdFrameKind::NewConnectionId, 1, 2, connectionId, token, 0 }).IsOk());
    Require(!BuildQuicConnectionIdFrame({
        QuicConnectionIdFrameKind::NewConnectionId, 1, 0, {}, token, 0 }).IsOk());
}
