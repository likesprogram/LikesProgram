#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Quic/QuicStreamEventMapping.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicStreamFrameTests() {
    using namespace LikesProgram::Quic;

    const std::array<std::uint8_t, 3> payload{ 0xA1, 0xB2, 0xC3 };
    QuicStreamFrameBuildOptions options;
    options.streamId = 7;
    options.offset = 37;
    options.hasOffset = true;
    options.hasLength = true;
    options.fin = true;
    options.data = payload;
    const auto encoded = BuildQuicStreamFrame(options);
    Require(encoded.IsOk());
    Require(encoded.Value().front() == 0x0F);
    const std::array<std::uint8_t, 1> trailing{ 0xFF };
    std::vector<std::uint8_t> withTrailing = encoded.Value();
    // Avoid GCC 15's false array-bounds diagnostic for one-byte range inserts.
    withTrailing.push_back(trailing.front());
    const auto decoded = ParseQuicStreamFrame(withTrailing.data(), withTrailing.size());
    Require(decoded.IsOk());
    Require(decoded.Value().streamId == 7);
    Require(decoded.Value().offset == 37);
    Require(decoded.Value().hasOffset && decoded.Value().hasLength && decoded.Value().fin);
    Require(decoded.Value().consumedBytes == encoded.Value().size());
    Require(std::vector<std::uint8_t>(
        decoded.Value().data.begin(), decoded.Value().data.end())
        == std::vector<std::uint8_t>(payload.begin(), payload.end()));

    QuicStreamFrameBuildOptions implicitLength;
    implicitLength.streamId = 3;
    implicitLength.data = payload;
    implicitLength.hasLength = false;
    const auto implicitEncoded = BuildQuicStreamFrame(implicitLength);
    Require(implicitEncoded.IsOk());
    const auto implicitDecoded = ParseQuicStreamFrame(
        implicitEncoded.Value().data(), implicitEncoded.Value().size());
    Require(implicitDecoded.IsOk());
    Require(!implicitDecoded.Value().hasOffset && !implicitDecoded.Value().hasLength);
    Require(implicitDecoded.Value().data.size() == payload.size());

    const std::array<std::uint8_t, 7> nonMinimalType{
        0x40, 0x0F, 0x00, 0x40, 0x25, 0x01, 0xAA };
    const auto nonMinimalDecoded = ParseQuicStreamFrame(
        nonMinimalType.data(), nonMinimalType.size());
    Require(nonMinimalDecoded.IsOk());
    Require(nonMinimalDecoded.Value().streamId == 0);
    Require(nonMinimalDecoded.Value().offset == 37);
    Require(nonMinimalDecoded.Value().data.size() == 1);

    Require(!ParseQuicStreamFrame(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> wrongType{ 0x02 };
    Require(!ParseQuicStreamFrame(wrongType.data(), wrongType.size()).IsOk());
    const std::array<std::uint8_t, 3> tooShort{
        0x0C, 0x00, 0x40 };
    Require(!ParseQuicStreamFrame(tooShort.data(), tooShort.size()).IsOk());
    const std::array<std::uint8_t, 4> tooLong{
        0x0A, 0x00, 0x05, 0xAA };
    Require(!ParseQuicStreamFrame(tooLong.data(), tooLong.size()).IsOk());
    options.hasOffset = false;
    options.offset = 1;
    Require(!BuildQuicStreamFrame(options).IsOk());

    const Address peer;
    const auto dataEvent = MapQuicStreamFrameToEvent(decoded.Value(), peer);
    Require(dataEvent.IsOk());
    Require(dataEvent.Value().kind == QuicEventKind::StreamFin);
    Require(dataEvent.Value().streamId == 7);
    Require(dataEvent.Value().payload.ReadableBytes() == payload.size());
    Require(std::vector<std::uint8_t>(
        dataEvent.Value().payload.Peek(),
        dataEvent.Value().payload.Peek() + dataEvent.Value().payload.ReadableBytes())
        == std::vector<std::uint8_t>(payload.begin(), payload.end()));

    const auto plainEvent = MapQuicStreamFrameToEvent({
        3, 0, false, true, false, {}, 0 }, peer);
    Require(plainEvent.IsOk());
    Require(plainEvent.Value().kind == QuicEventKind::StreamData);
    Require(plainEvent.Value().streamId == 3);
    Require(plainEvent.Value().payload.ReadableBytes() == 0);
}
