#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamControlAction.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicStreamControlFrameTests() {
    using namespace LikesProgram::Quic;

    QuicResetStreamFrame reset{ 7, 0x10, 4096, 0 };
    const auto resetEncoded = BuildQuicResetStreamFrame(reset);
    Require(resetEncoded.IsOk());
    const std::array<std::uint8_t, 1> trailing{ 0xFF };
    std::vector<std::uint8_t> resetInput = resetEncoded.Value();
    // Avoid GCC 15's false array-bounds diagnostic for one-byte range inserts.
    resetInput.push_back(trailing.front());
    const auto resetDecoded = ParseQuicResetStreamFrame(
        resetInput.data(), resetInput.size());
    Require(resetDecoded.IsOk());
    Require(resetDecoded.Value().streamId == reset.streamId);
    Require(resetDecoded.Value().applicationErrorCode == reset.applicationErrorCode);
    Require(resetDecoded.Value().finalSize == reset.finalSize);
    Require(resetDecoded.Value().consumedBytes == resetEncoded.Value().size());

    QuicStopSendingFrame stop{ 9, 0x22, 0 };
    const auto stopEncoded = BuildQuicStopSendingFrame(stop);
    Require(stopEncoded.IsOk());
    const auto stopDecoded = ParseQuicStopSendingFrame(
        stopEncoded.Value().data(), stopEncoded.Value().size());
    Require(stopDecoded.IsOk());
    Require(stopDecoded.Value().streamId == stop.streamId);
    Require(stopDecoded.Value().applicationErrorCode == stop.applicationErrorCode);

    const std::array<std::uint8_t, 7> nonMinimalReset{
        0x40, 0x04, 0x40, 0x25, 0x01, 0x40, 0x01 };
    Require(ParseQuicResetStreamFrame(
        nonMinimalReset.data(), nonMinimalReset.size()).IsOk());
    Require(!ParseQuicResetStreamFrame(nullptr, 0).IsOk());
    Require(!ParseQuicStopSendingFrame(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> wrongType{ 0x02 };
    Require(!ParseQuicResetStreamFrame(wrongType.data(), wrongType.size()).IsOk());
    Require(!ParseQuicStopSendingFrame(wrongType.data(), wrongType.size()).IsOk());
    const std::array<std::uint8_t, 2> truncated{ 0x04, 0x00 };
    Require(!ParseQuicResetStreamFrame(truncated.data(), truncated.size()).IsOk());
    Require(!ParseQuicStopSendingFrame(truncated.data(), truncated.size()).IsOk());

    const auto resetAction = BuildQuicResetStreamAction(reset);
    Require(resetAction.IsOk());
    Require(resetAction.Value().kind == QuicActionKind::ResetStream);
    Require(resetAction.Value().actionId == 0);
    Require(resetAction.Value().streamId == reset.streamId);
    Require(resetAction.Value().errorCode == reset.applicationErrorCode);
    Require(resetAction.Value().payload.ReadableBytes() == resetEncoded.Value().size());
    Require(!BuildQuicResetStreamAction({
        0, kQuicVarIntMaximum + 1, 0, 0 }).IsOk());

    const auto stopAction = BuildQuicStopSendingAction(stop);
    Require(stopAction.IsOk());
    Require(stopAction.Value().kind == QuicActionKind::StopSending);
    Require(stopAction.Value().actionId == 0);
    Require(stopAction.Value().streamId == stop.streamId);
    Require(stopAction.Value().errorCode == stop.applicationErrorCode);
    Require(stopAction.Value().payload.ReadableBytes() == stopEncoded.Value().size());
    Require(!BuildQuicStopSendingAction({
        0, kQuicVarIntMaximum + 1, 0 }).IsOk());
}
