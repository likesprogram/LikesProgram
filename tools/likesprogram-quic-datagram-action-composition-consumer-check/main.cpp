#include <LikesProgram/Quic/QuicDatagramFrame.hpp>
#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>

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
        QuicActionMessage copy(QuicActionKind::DatagramReady);
        copy.actionId = actionId;
        copy.payload = MakeBuffer(source.payload.AsStringView());
        return copy;
    }

    class DatagramSink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if (action.kind != QuicActionKind::DatagramReady
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

    QuicActionMessage MakeDatagramAction(const std::vector<std::uint8_t>& bytes) {
        QuicActionMessage action(QuicActionKind::DatagramReady);
        action.payload.Append(bytes.data(), bytes.size());
        return action;
    }
}

int Run() {
    using namespace LikesProgram::Quic;
    const std::vector<std::uint8_t> payload{ 'h', 'e', 'l', 'l', 'o' };
    const auto lengthBearing = BuildQuicDatagramFrame(true, payload);
    Require(lengthBearing.IsOk(),
        "length-bearing DATAGRAM should build");
    const auto parsedLengthBearing = ParseQuicDatagramFrame(
        lengthBearing.Value().data(), lengthBearing.Value().size());
    Require(parsedLengthBearing.IsOk()
        && parsedLengthBearing.Value().hasLength
        && parsedLengthBearing.Value().data.size() == payload.size()
        && std::string_view(reinterpret_cast<const char*>(
            parsedLengthBearing.Value().data.data()), payload.size()) == "hello"
        && parsedLengthBearing.Value().consumedBytes == lengthBearing.Value().size(),
        "length-bearing DATAGRAM should preserve payload and boundary");

    const std::vector<std::uint8_t> raw{ 0xaa, 0xbb };
    const auto lengthless = BuildQuicDatagramFrame(false, raw);
    Require(lengthless.IsOk(),
        "lengthless DATAGRAM should build");
    const auto parsedLengthless = ParseQuicDatagramFrame(
        lengthless.Value().data(), lengthless.Value().size());
    Require(parsedLengthless.IsOk() && !parsedLengthless.Value().hasLength
        && parsedLengthless.Value().data.size() == raw.size()
        && parsedLengthless.Value().consumedBytes == lengthless.Value().size(),
        "lengthless DATAGRAM should consume the enclosing payload remainder");

    const auto ping = BuildQuicPingFrame();
    Require(ping.IsOk() && ping.Value().size() == 1,
        "PING should build to one wire byte");
    const auto parsedPing = ParseQuicPingFrame(ping.Value().data(), ping.Value().size());
    Require(parsedPing.IsOk() && parsedPing.Value().consumedBytes == 1,
        "PING should parse its consumed boundary");

    DatagramSink sink;
    const auto acceptedDatagram = sink.Submit(
        CopyAction(MakeDatagramAction(lengthBearing.Value()), 1));
    Require(acceptedDatagram.Accepted() && acceptedDatagram.actionId == 1,
        "DATAGRAM action should be accepted with an external id");
    sink.nextState = QuicActionDeliveryState::Deferred;
    sink.nextError = QuicActionDeliveryError::Backpressure;
    const auto deferredPing = sink.Submit(
        CopyAction(MakeDatagramAction(ping.Value()), 2));
    Require(deferredPing.Deferred() && deferredPing.actionId == 2,
        "PING action should preserve deferred delivery metadata");
    const auto retryPing = sink.Submit(
        CopyAction(MakeDatagramAction(ping.Value()), 3));
    Require(retryPing.Accepted() && retryPing.actionId == 3
        && sink.received.size() == 3,
        "PING retry should use a fresh action id and own its payload");

    const std::uint8_t truncated[] = { 0x31, 0x05, 0x01 };
    Require(!ParseQuicDatagramFrame(truncated, sizeof(truncated)).IsOk(),
        "truncated length-bearing DATAGRAM should be rejected");
    const std::uint8_t wrongType[] = { 0x02 };
    Require(!ParseQuicPingFrame(wrongType, sizeof(wrongType)).IsOk(),
        "non-PING frame should be rejected by the PING parser");
    QuicActionMessage invalid(QuicActionKind::DatagramReady);
    invalid.actionId = 4;
    Require(sink.Submit(std::move(invalid)).Rejected()
        && sink.received.size() == 3,
        "empty datagram action should be rejected before delivery");

    std::cout << "passed=true"
              << " datagram_length=true"
              << " datagram_lengthless=true"
              << " ping_roundtrip=true"
              << " deferred_retry_ids=true"
              << " payload_owned=true"
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
