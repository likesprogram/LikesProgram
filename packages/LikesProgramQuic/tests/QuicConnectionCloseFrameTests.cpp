#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseEventMapping.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicConnectionCloseFrameTests() {
    using namespace LikesProgram::Quic;

    const std::array<std::uint8_t, 3> reason{ 'b', 'y', 'e' };
    QuicConnectionCloseFrame transport;
    transport.errorCode = 0x10;
    transport.frameType = 0x08;
    transport.reason = reason;
    const auto transportEncoded = BuildQuicConnectionCloseFrame(transport);
    Require(transportEncoded.IsOk());
    const auto transportDecoded = ParseQuicConnectionCloseFrame(
        transportEncoded.Value().data(), transportEncoded.Value().size());
    Require(transportDecoded.IsOk());
    Require(!transportDecoded.Value().application);
    Require(transportDecoded.Value().errorCode == transport.errorCode);
    Require(transportDecoded.Value().frameType == transport.frameType);
    Require(transportDecoded.Value().reason.size() == reason.size());

    QuicConnectionCloseFrame application;
    application.application = true;
    application.errorCode = 0x100;
    application.reason = reason;
    const auto applicationEncoded = BuildQuicConnectionCloseFrame(application);
    Require(applicationEncoded.IsOk());
    const auto applicationDecoded = ParseQuicConnectionCloseFrame(
        applicationEncoded.Value().data(), applicationEncoded.Value().size());
    Require(applicationDecoded.IsOk());
    Require(applicationDecoded.Value().application);
    Require(applicationDecoded.Value().errorCode == application.errorCode);
    Require(applicationDecoded.Value().frameType == 0);

    const std::array<std::uint8_t, 8> nonMinimal{
        0x40, 0x1C, 0x01, 0x08, 0x01, 'x', 0xAA, 0xBB };
    Require(ParseQuicConnectionCloseFrame(
        nonMinimal.data(), nonMinimal.size()).IsOk());
    Require(!ParseQuicConnectionCloseFrame(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> wrongType{ 0x02 };
    Require(!ParseQuicConnectionCloseFrame(wrongType.data(), wrongType.size()).IsOk());
    const std::array<std::uint8_t, 2> truncated{ 0x1C, 0x00 };
    Require(!ParseQuicConnectionCloseFrame(truncated.data(), truncated.size()).IsOk());

    const Address peer;
    const auto transportEvent = MapQuicConnectionCloseFrameToEvent(transport, peer);
    Require(transportEvent.IsOk());
    Require(transportEvent.Value().kind == QuicEventKind::ConnectionClose);
    Require(transportEvent.Value().errorCode == transport.errorCode);
    Require(!transportEvent.Value().applicationError);
    Require(transportEvent.Value().payload.ReadableBytes() == reason.size());

    const auto applicationEvent = MapQuicConnectionCloseFrameToEvent(application, peer);
    Require(applicationEvent.IsOk());
    Require(applicationEvent.Value().applicationError);
    Require(applicationEvent.Value().errorCode == application.errorCode);
}
