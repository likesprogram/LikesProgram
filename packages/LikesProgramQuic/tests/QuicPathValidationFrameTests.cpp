#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Quic/QuicPathValidationAction.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicPathValidationFrameTests() {
    using namespace LikesProgram::Quic;
    const std::array<std::uint8_t, 8> token{ 0, 1, 2, 3, 4, 5, 6, 7 };
    for (const auto kind : { QuicPathValidationFrameKind::PathChallenge,
        QuicPathValidationFrameKind::PathResponse }) {
        const auto encoded = BuildQuicPathValidationFrame({ kind, token, 0 });
        Require(encoded.IsOk());
        std::vector<std::uint8_t> input = encoded.Value();
        input.push_back(0xA5);
        const auto parsed = ParseQuicPathValidationFrame(input.data(), input.size());
        Require(parsed.IsOk());
        Require(parsed.Value().kind == kind);
        Require(parsed.Value().data == token);
        Require(parsed.Value().consumedBytes == encoded.Value().size());
    }

    const std::uint8_t nonMinimal[] = { 0x40, 0x1a, 0, 1, 2, 3, 4, 5, 6, 7 };
    const auto parsed = ParseQuicPathValidationFrame(nonMinimal, sizeof(nonMinimal));
    Require(parsed.IsOk());
    Require(parsed.Value().kind == QuicPathValidationFrameKind::PathChallenge);
    Require(parsed.Value().consumedBytes == sizeof(nonMinimal));

    const std::uint8_t wrongType[] = { 0x10, 0, 1, 2, 3, 4, 5, 6, 7 };
    Require(!ParseQuicPathValidationFrame(wrongType, sizeof(wrongType)).IsOk());
    const std::uint8_t truncated[] = { 0x1a, 0, 1, 2 };
    Require(!ParseQuicPathValidationFrame(truncated, sizeof(truncated)).IsOk());
    Require(!BuildQuicPathValidationFrame({
        static_cast<QuicPathValidationFrameKind>(99), token, 0 }).IsOk());

    const auto response = BuildQuicPathResponseFrame({
        QuicPathValidationFrameKind::PathChallenge, token, 0 });
    Require(response.IsOk());
    Require(response.Value().kind == QuicPathValidationFrameKind::PathResponse);
    Require(response.Value().data == token);
    const auto responseWire = BuildQuicPathValidationFrame(response.Value());
    Require(responseWire.IsOk());
    Require(responseWire.Value().front() == 0x1b);

    Require(!BuildQuicPathResponseFrame({
        QuicPathValidationFrameKind::PathResponse, token, 0 }).IsOk());

    const auto responseAction = BuildQuicPathResponseAction({
        QuicPathValidationFrameKind::PathChallenge, token, 0 });
    Require(responseAction.IsOk());
    Require(responseAction.Value().kind == QuicActionKind::PathResponse);
    Require(responseAction.Value().actionId == 0);
    Require(responseAction.Value().payload.ReadableBytes() == 9);
    Require(!BuildQuicPathResponseAction({
        QuicPathValidationFrameKind::PathResponse, token, 0 }).IsOk());
}
