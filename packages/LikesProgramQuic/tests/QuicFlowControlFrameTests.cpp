#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>

#include <cstdint>
#include <cstdlib>
#include <iterator>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition) {
        if (!condition) std::abort();
    }

    void RoundTrip(QuicFlowControlFrame frame) {
        const auto encoded = BuildQuicFlowControlFrame(frame);
        Require(encoded.IsOk());
        const std::uint8_t trailing[] = { 0xA5 };
        std::vector<std::uint8_t> input = encoded.Value();
        // Avoid GCC 15's false array-bounds diagnostic for one-byte range inserts.
        input.push_back(trailing[0]);
        const auto parsed = ParseQuicFlowControlFrame(input.data(), input.size());
        Require(parsed.IsOk());
        Require(parsed.Value().kind == frame.kind);
        Require(parsed.Value().streamId == frame.streamId);
        Require(parsed.Value().limit == frame.limit);
        Require(parsed.Value().consumedBytes == encoded.Value().size());
    }
}

void RunQuicFlowControlFrameTests() {
    RoundTrip({ QuicFlowControlFrameKind::MaxData, 0, 1024, 0 });
    RoundTrip({ QuicFlowControlFrameKind::MaxStreamData, 7, 2048, 0 });
    RoundTrip({ QuicFlowControlFrameKind::MaxStreamsBidi, 0, 3, 0 });
    RoundTrip({ QuicFlowControlFrameKind::MaxStreamsUni, 0, 4, 0 });
    RoundTrip({ QuicFlowControlFrameKind::DataBlocked, 0, 1024, 0 });
    RoundTrip({ QuicFlowControlFrameKind::StreamDataBlocked, 9, 4096, 0 });
    RoundTrip({ QuicFlowControlFrameKind::StreamsBlockedBidi, 0, 5, 0 });
    RoundTrip({ QuicFlowControlFrameKind::StreamsBlockedUni, 0, 6, 0 });

    const std::uint8_t nonMinimal[] = { 0x40, 0x10, 0x40, 0x01 };
    const auto parsed = ParseQuicFlowControlFrame(nonMinimal, sizeof(nonMinimal));
    Require(parsed.IsOk());
    Require(parsed.Value().kind == QuicFlowControlFrameKind::MaxData);
    Require(parsed.Value().limit == 1);
    Require(parsed.Value().consumedBytes == 4);

    const std::uint8_t wrongType[] = { 0x08, 0x00, 0x00 };
    Require(!ParseQuicFlowControlFrame(wrongType, sizeof(wrongType)).IsOk());
    const std::uint8_t truncated[] = { 0x11, 0x40 };
    Require(!ParseQuicFlowControlFrame(truncated, sizeof(truncated)).IsOk());
    Require(!BuildQuicFlowControlFrame({
        static_cast<QuicFlowControlFrameKind>(99), 0, 1, 0 }).IsOk());
}
