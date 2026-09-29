#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>

#include <array>
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

    LikesProgram::Result<QuicActionMessage> BuildCloseAction(
        const QuicConnectionCloseFrame& frame) {
        const auto encoded = BuildQuicConnectionCloseFrame(frame);
        if (!encoded.IsOk()) return encoded.GetStatus();
        QuicActionMessage action(
            QuicActionKind::CloseConnection, 0, frame.errorCode);
        action.payload.Append(encoded.Value().data(), encoded.Value().size());
        return action;
    }

    QuicActionMessage CopyAction(const QuicActionMessage& source,
        std::uint64_t actionId) {
        QuicActionMessage copy(source.kind, source.streamId, source.errorCode);
        copy.actionId = actionId;
        copy.payload = MakeBuffer(source.payload.AsStringView());
        return copy;
    }

    class CloseSink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if (action.kind != QuicActionKind::CloseConnection
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

    LikesProgram::Result<QuicConnectionCloseFrame> ParsePayload(
        const QuicActionMessage& action) {
        const auto payload = action.payload.AsStringView();
        return ParseQuicConnectionCloseFrame(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    std::array<std::uint8_t, 3> transportReason{ 'b', 'y', 'e' };
    QuicConnectionCloseFrame transport;
    transport.errorCode = 0x10;
    transport.frameType = 0x08;
    transport.reason = transportReason;
    const auto transportAction = BuildCloseAction(transport);
    Require(transportAction.IsOk()
        && transportAction.Value().kind == QuicActionKind::CloseConnection
        && transportAction.Value().errorCode == 0x10
        && transportAction.Value().actionId == 0,
        "transport close should build a complete unassigned action");
    transportReason = { 'b', 'a', 'd' };

    CloseSink sink;
    auto first = CopyAction(transportAction.Value(), 1);
    const auto acceptedTransport = sink.Submit(std::move(first));
    Require(acceptedTransport.Accepted() && acceptedTransport.actionId == 1,
        "transport close should be accepted with its external action id");
    const auto parsedTransport = ParsePayload(sink.received.back());
    Require(parsedTransport.IsOk() && !parsedTransport.Value().application
        && parsedTransport.Value().errorCode == 0x10
        && parsedTransport.Value().frameType == 0x08
        && std::string_view(reinterpret_cast<const char*>(
            parsedTransport.Value().reason.data()), parsedTransport.Value().reason.size()) == "bye",
        "transport close payload should preserve copied metadata and reason bytes");
    const auto transportEvent = MapQuicConnectionCloseFrameToEvent(
        parsedTransport.Value(), {});
    Require(transportEvent.IsOk()
        && transportEvent.Value().kind == QuicEventKind::ConnectionClose
        && !transportEvent.Value().applicationError
        && transportEvent.Value().errorCode == 0x10
        && transportEvent.Value().payload.AsStringView() == "bye",
        "accepted transport payload should map to an owning close event");

    const std::array<std::uint8_t, 3> applicationReason{ 'a', 'p', 'p' };
    QuicConnectionCloseFrame application;
    application.application = true;
    application.errorCode = 0x100;
    application.reason = applicationReason;
    const auto applicationAction = BuildCloseAction(application);
    Require(applicationAction.IsOk(),
        "application close should build a complete action");
    sink.nextState = QuicActionDeliveryState::Rejected;
    sink.nextError = QuicActionDeliveryError::TransportUnavailable;
    auto rejectedAction = CopyAction(applicationAction.Value(), 2);
    const auto rejected = sink.Submit(std::move(rejectedAction));
    Require(rejected.Rejected()
        && rejected.error == QuicActionDeliveryError::TransportUnavailable
        && rejected.actionId == 2,
        "transport rejection should preserve the close action id");
    auto retryAction = CopyAction(applicationAction.Value(), 3);
    const auto acceptedRetry = sink.Submit(std::move(retryAction));
    Require(acceptedRetry.Accepted() && acceptedRetry.actionId == 3,
        "caller retry should use a fresh close action id");
    const auto parsedApplication = ParsePayload(sink.received.back());
    Require(parsedApplication.IsOk() && parsedApplication.Value().application
        && parsedApplication.Value().errorCode == 0x100
        && parsedApplication.Value().frameType == 0,
        "application close should preserve its wire kind and error");
    const auto applicationEvent = MapQuicConnectionCloseFrameToEvent(
        parsedApplication.Value(), {});
    Require(applicationEvent.IsOk()
        && applicationEvent.Value().applicationError
        && applicationEvent.Value().payload.AsStringView() == "app",
        "accepted application close should map to its event metadata");

    QuicConnectionCloseFrame invalid;
    invalid.errorCode = std::uint64_t{ 1 } << 62;
    Require(!BuildCloseAction(invalid).IsOk() && sink.received.size() == 3,
        "invalid close values should not reach the sink");
    const std::array<std::uint8_t, 2> truncated{ 0x1c, 0x00 };
    Require(!ParseQuicConnectionCloseFrame(truncated.data(), truncated.size()).IsOk(),
        "truncated close payload should be rejected");
    QuicActionMessage invalidAction(QuicActionKind::CloseConnection);
    Require(sink.Submit(std::move(invalidAction)).Rejected(),
        "zero-id empty close actions should be rejected");

    std::cout << "passed=true"
              << " transport_close=true"
              << " application_close=true"
              << " rejected_retry_ids=true"
              << " reason_owned=true"
              << " event_mapped=true"
              << " invalid_rejected=true\n";
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
