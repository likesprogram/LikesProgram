#include <LikesProgram/Quic/QuicStreamControlAction.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamEventMapping.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Quic/QuicStreamTerminalTracker.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::vector<std::uint8_t> ReadPayload(const Buffer& payload) {
        return { payload.Peek(), payload.Peek() + payload.ReadableBytes() };
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::uint64_t streamId = 8;
    const std::array<std::uint8_t, 2> finalPayload{ 0x11, 0x22 };
    const auto encodedStream = BuildQuicStreamFrame({
        streamId, 0, true, true, true, finalPayload });
    Require(encodedStream.IsOk(), "FIN STREAM should build");
    const auto parsedStream = ParseQuicStreamFrame(
        encodedStream.Value().data(), encodedStream.Value().size());
    Require(parsedStream.IsOk() && parsedStream.Value().fin
        && parsedStream.Value().offset == 0
        && parsedStream.Value().data.size() == finalPayload.size(),
        "FIN STREAM should preserve final payload coordinates");

    const Address peer;
    const auto event = MapQuicStreamFrameToEvent(parsedStream.Value(), peer);
    Require(event.IsOk() && event.Value().kind == QuicEventKind::StreamFin
        && event.Value().streamId == streamId
        && ReadPayload(event.Value().payload)
            == std::vector<std::uint8_t>(finalPayload.begin(), finalPayload.end()),
        "FIN STREAM should map to an owning StreamFin event");

    QuicStreamTerminalState finState;
    const auto fin = ObserveQuicStreamFin(finState, finalPayload.size());
    const auto duplicateFin = ObserveQuicStreamFin(finState, finalPayload.size());
    const auto mismatchFin = ObserveQuicStreamFin(finState, finalPayload.size() + 1);
    Require(fin.Succeeded() && fin.advanced && duplicateFin.Succeeded()
        && !duplicateFin.advanced
        && mismatchFin.error == QuicStreamTerminalError::FinalSizeMismatch
        && finState.finObserved && finState.finalSize == finalPayload.size(),
        "FIN duplicate and final-size mismatch must be deterministic");

    const QuicResetStreamFrame resetFrame{ streamId, 0x55, finalPayload.size(), 0 };
    const auto resetWire = BuildQuicResetStreamFrame(resetFrame);
    const auto resetAction = BuildQuicResetStreamAction(resetFrame);
    Require(resetWire.IsOk() && resetAction.IsOk()
        && resetAction.Value().kind == QuicActionKind::ResetStream
        && resetAction.Value().actionId == 0
        && resetAction.Value().streamId == streamId
        && resetAction.Value().errorCode == resetFrame.applicationErrorCode
        && resetAction.Value().payload.ReadableBytes() == resetWire.Value().size(),
        "RESET_STREAM action should keep id external and payload owned");
    auto callerAssignedReset = resetAction.Value();
    callerAssignedReset.actionId = 91;
    Require(callerAssignedReset.actionId != 0
        && callerAssignedReset.payload.ReadableBytes()
            == resetAction.Value().payload.ReadableBytes(),
        "caller may assign delivery id without changing wire payload");

    const auto parsedReset = ParseQuicResetStreamFrame(
        resetWire.Value().data(), resetWire.Value().size());
    Require(parsedReset.IsOk() && parsedReset.Value().finalSize == finalPayload.size(),
        "RESET_STREAM should round trip final size");
    const auto resetAfterFin = ObserveQuicStreamReset(
        finState, resetFrame.applicationErrorCode, resetFrame.finalSize);
    Require(resetAfterFin.error == QuicStreamTerminalError::ResetAfterFin
        && finState.finObserved && !finState.resetObserved,
        "RESET_STREAM after FIN must preserve terminal state");

    QuicStreamTerminalState resetState;
    const auto resetObserved = ObserveQuicStreamReset(
        resetState, resetFrame.applicationErrorCode, resetFrame.finalSize);
    const auto duplicateReset = ObserveQuicStreamReset(
        resetState, resetFrame.applicationErrorCode, resetFrame.finalSize);
    const auto resetMismatch = ObserveQuicStreamReset(
        resetState, resetFrame.applicationErrorCode + 1, resetFrame.finalSize);
    Require(resetObserved.Succeeded() && resetObserved.advanced
        && duplicateReset.Succeeded() && !duplicateReset.advanced
        && resetMismatch.error == QuicStreamTerminalError::ResetCodeMismatch
        && resetState.resetObserved && resetState.resetErrorCode
            == resetFrame.applicationErrorCode,
        "RESET duplicate and code mismatch must preserve state");

    const QuicStopSendingFrame stopFrame{ streamId, 0x66, 0 };
    const auto stopWire = BuildQuicStopSendingFrame(stopFrame);
    const auto stopAction = BuildQuicStopSendingAction(stopFrame);
    Require(stopWire.IsOk() && stopAction.IsOk()
        && stopAction.Value().kind == QuicActionKind::StopSending
        && stopAction.Value().actionId == 0
        && stopAction.Value().streamId == streamId
        && stopAction.Value().errorCode == stopFrame.applicationErrorCode,
        "STOP_SENDING action should preserve stream/error metadata");
    const auto parsedStop = ParseQuicStopSendingFrame(
        stopWire.Value().data(), stopWire.Value().size());
    Require(parsedStop.IsOk() && parsedStop.Value().streamId == streamId,
        "STOP_SENDING should round trip stream metadata");
    const auto stopObserved = ObserveQuicStreamStopSending(
        resetState, stopFrame.applicationErrorCode);
    const auto stopDuplicate = ObserveQuicStreamStopSending(
        resetState, stopFrame.applicationErrorCode);
    const auto stopMismatch = ObserveQuicStreamStopSending(
        resetState, stopFrame.applicationErrorCode + 1);
    Require(stopObserved.Succeeded() && stopDuplicate.Succeeded()
        && !stopDuplicate.advanced
        && stopMismatch.error == QuicStreamTerminalError::StopSendingCodeMismatch
        && resetState.stopSendingObserved,
        "STOP_SENDING duplicate and mismatch must preserve state");

    std::vector<std::uint8_t> truncatedReset = resetWire.Value();
    truncatedReset.pop_back();
    const auto beforeMalformed = resetState;
    Require(!ParseQuicResetStreamFrame(
        truncatedReset.data(), truncatedReset.size()).IsOk(),
        "truncated RESET_STREAM must be rejected");
    Require(resetState.finObserved == beforeMalformed.finObserved
        && resetState.resetObserved == beforeMalformed.resetObserved
        && resetState.stopSendingObserved == beforeMalformed.stopSendingObserved
        && resetState.finalSize == beforeMalformed.finalSize
        && resetState.resetErrorCode == beforeMalformed.resetErrorCode
        && resetState.stopSendingErrorCode == beforeMalformed.stopSendingErrorCode,
        "malformed terminal input must not mutate state");

    resetState.Reset();
    Require(!resetState.finObserved && !resetState.resetObserved
        && !resetState.stopSendingObserved,
        "Reset must clear terminal observations");

    std::cout << "passed=true"
              << " fin_event=true"
              << " final_size=true"
              << " reset_rejects_after_fin=true"
              << " reset_duplicate=true"
              << " stop_action=true"
              << " stop_mismatch=true"
              << " malformed_preserves=true"
              << " action_id_external=true"
              << " reset=true\n";
    return 0;
}

int main() {
    try {
        return Run();
    }
    catch (const std::exception& error) {
        std::cerr << "failed=" << error.what() << '\n';
        return 99;
    }
}
