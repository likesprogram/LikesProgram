#include <LikesProgram/Quic/QuicAckFrame.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

void RunQuicAckFrameTests() {
    using namespace LikesProgram::Quic;

    QuicAckFrame frame;
    frame.largestAcknowledged = 120;
    frame.ackDelay = 0x12;
    frame.ranges = { { 100, 120 }, { 90, 95 }, { 80, 80 } };
    const auto encoded = BuildQuicAckFrame(frame);
    Require(encoded.IsOk());
    Require(encoded.Value().front() == 0x02);
    const std::array<std::uint8_t, 2> trailing{ 0xAA, 0xBB };
    std::vector<std::uint8_t> withTrailing = encoded.Value();
    withTrailing.insert(withTrailing.end(), trailing.begin(), trailing.end());
    const auto decoded = ParseQuicAckFrame(withTrailing.data(), withTrailing.size());
    Require(decoded.IsOk());
    Require(decoded.Value().consumedBytes == encoded.Value().size());
    Require(!decoded.Value().ecn);
    Require(decoded.Value().largestAcknowledged == frame.largestAcknowledged);
    Require(decoded.Value().ackDelay == frame.ackDelay);
    Require(decoded.Value().ranges == frame.ranges);

    frame.ecn = true;
    frame.ect0Count = 7;
    frame.ect1Count = 8;
    frame.ecnCeCount = 9;
    const auto ecnEncoded = BuildQuicAckFrame(frame);
    Require(ecnEncoded.IsOk());
    Require(ecnEncoded.Value().front() == 0x03);
    const auto ecnDecoded = ParseQuicAckFrame(
        ecnEncoded.Value().data(), ecnEncoded.Value().size());
    Require(ecnDecoded.IsOk());
    Require(ecnDecoded.Value().ecn == frame.ecn);
    Require(ecnDecoded.Value().ranges == frame.ranges);
    Require(ecnDecoded.Value().ect0Count == frame.ect0Count);
    Require(ecnDecoded.Value().ect1Count == frame.ect1Count);
    Require(ecnDecoded.Value().ecnCeCount == frame.ecnCeCount);

    const std::array<std::uint8_t, 6> nonMinimalType{
        0x40, 0x02, 0x00, 0x00, 0x00, 0x00 };
    Require(ParseQuicAckFrame(
        nonMinimalType.data(), nonMinimalType.size()).IsOk());
    Require(!ParseQuicAckFrame(nullptr, 0).IsOk());
    const std::array<std::uint8_t, 1> wrongType{ 0x01 };
    Require(!ParseQuicAckFrame(wrongType.data(), wrongType.size()).IsOk());
    QuicAckFrame invalid = frame;
    invalid.ranges.clear();
    Require(!BuildQuicAckFrame(invalid).IsOk());
    invalid = frame;
    invalid.ranges = { { 100, 120 }, { 99, 99 } };
    Require(!BuildQuicAckFrame(invalid).IsOk());
    invalid = frame;
    invalid.ranges = { { 100, 120 }, { 90, 121 } };
    Require(!BuildQuicAckFrame(invalid).IsOk());

    const std::array<std::uint8_t, 5> underflow{
        0x02, 0x01, 0x00, 0x01, 0x01 };
    Require(!ParseQuicAckFrame(underflow.data(), underflow.size()).IsOk());
}
