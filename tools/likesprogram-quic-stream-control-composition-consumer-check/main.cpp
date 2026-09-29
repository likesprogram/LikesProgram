#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicStreamControlAction.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;
    using LikesProgram::Net::Buffer;

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    Buffer MakeBuffer(std::string_view value) {
        Buffer buffer(0);
        buffer.Append(value.data(), value.size());
        return buffer;
    }

    QuicActionMessage CopyAction(const QuicActionMessage& source,
        std::uint64_t actionId) {
        QuicActionMessage copy(source.kind, source.streamId, source.errorCode);
        copy.actionId = actionId;
        copy.payload = MakeBuffer(source.payload.AsStringView());
        return copy;
    }

    class ControlSink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if ((action.kind != QuicActionKind::ResetStream
                    && action.kind != QuicActionKind::StopSending)
                || action.actionId == 0 || action.payload.ReadableBytes() == 0) {
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::InvalidAction, action.actionId };
            }
            const auto id = action.actionId;
            received.push_back(std::move(action));
            const auto delivery = QuicActionDelivery{ nextState, nextError, id };
            nextState = QuicActionDeliveryState::Accepted;
            nextError = QuicActionDeliveryError::None;
            return delivery;
        }

        std::vector<QuicActionMessage> received;
        QuicActionDeliveryState nextState = QuicActionDeliveryState::Accepted;
        QuicActionDeliveryError nextError = QuicActionDeliveryError::None;
    };

    LikesProgram::Result<QuicResetStreamFrame> ParseReset(
        const QuicActionMessage& action) {
        const auto payload = action.payload.AsStringView();
        return ParseQuicResetStreamFrame(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    }

    LikesProgram::Result<QuicStopSendingFrame> ParseStop(
        const QuicActionMessage& action) {
        const auto payload = action.payload.AsStringView();
        return ParseQuicStopSendingFrame(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const QuicResetStreamFrame reset{ 7, 0x10, 4096, 0 };
    const auto resetAction = BuildQuicResetStreamAction(reset);
    Require(resetAction.IsOk()
        && resetAction.Value().kind == QuicActionKind::ResetStream
        && resetAction.Value().actionId == 0
        && resetAction.Value().streamId == reset.streamId
        && resetAction.Value().errorCode == reset.applicationErrorCode,
        "RESET_STREAM should map to a zero-id control action");

    const QuicStopSendingFrame stop{ 9, 0x22, 0 };
    const auto stopAction = BuildQuicStopSendingAction(stop);
    Require(stopAction.IsOk()
        && stopAction.Value().kind == QuicActionKind::StopSending
        && stopAction.Value().actionId == 0
        && stopAction.Value().streamId == stop.streamId
        && stopAction.Value().errorCode == stop.applicationErrorCode,
        "STOP_SENDING should map to a zero-id control action");

    ControlSink sink;
    const auto resetDelivery = sink.Submit(CopyAction(resetAction.Value(), 1));
    Require(resetDelivery.Accepted() && resetDelivery.actionId == 1,
        "RESET_STREAM should be accepted with a caller id");
    const auto parsedReset = ParseReset(sink.received.back());
    Require(parsedReset.IsOk()
        && parsedReset.Value().streamId == reset.streamId
        && parsedReset.Value().applicationErrorCode == reset.applicationErrorCode
        && parsedReset.Value().finalSize == reset.finalSize,
        "accepted RESET_STREAM payload should preserve all coordinates");

    sink.nextState = QuicActionDeliveryState::Deferred;
    sink.nextError = QuicActionDeliveryError::Backpressure;
    const auto stopDeferred = sink.Submit(CopyAction(stopAction.Value(), 2));
    Require(stopDeferred.Deferred() && stopDeferred.actionId == 2,
        "STOP_SENDING backpressure should preserve its action id");
    const auto stopRetry = sink.Submit(CopyAction(stopAction.Value(), 3));
    Require(stopRetry.Accepted() && stopRetry.actionId == 3,
        "STOP_SENDING retry should use a fresh action id");
    const auto parsedStop = ParseStop(sink.received.back());
    Require(parsedStop.IsOk()
        && parsedStop.Value().streamId == stop.streamId
        && parsedStop.Value().applicationErrorCode == stop.applicationErrorCode,
        "accepted STOP_SENDING payload should preserve stream/error metadata");

    const std::uint64_t invalidValue = std::uint64_t{ 1 } << 62;
    Require(!BuildQuicResetStreamAction({ 0, invalidValue, 0, 0 }).IsOk()
        && !BuildQuicStopSendingAction({ 0, invalidValue, 0 }).IsOk(),
        "invalid control values should be refused before delivery");
    QuicActionMessage wrongKind(QuicActionKind::DatagramReady);
    wrongKind.actionId = 4;
    Require(sink.Submit(std::move(wrongKind)).Rejected()
        && sink.received.size() == 3,
        "wrong action kind should not enter the control sink");
    const std::uint8_t truncated[] = { 0x04, 0x00 };
    Require(!ParseQuicResetStreamFrame(truncated, sizeof(truncated)).IsOk(),
        "truncated RESET_STREAM payload should be rejected");

    std::cout << "passed=true"
              << " reset_mapped=true"
              << " stop_mapped=true"
              << " metadata_roundtrip=true"
              << " deferred_retry_ids=true"
              << " invalid_rejected=true"
              << " sink_owned=3\n";
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
