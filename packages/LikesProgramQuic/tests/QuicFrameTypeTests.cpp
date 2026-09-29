#include <LikesProgram/Quic/QuicFrameType.hpp>

#include <array>
#include <stdexcept>

namespace {
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

void RunQuicFrameTypeTests() {
    using namespace LikesProgram::Quic;
    Require(ClassifyQuicFrameType(0x08) == QuicFrameType::Stream
        && ClassifyQuicFrameType(0x03) == QuicFrameType::Ack
        && ClassifyQuicFrameType(0x31) == QuicFrameType::Datagram,
        "QUIC frame type classification should cover flag variants");
    const std::array<std::uint8_t, 2> encoded{ 0x40, 0x2a };
    auto parsed = ParseQuicFrameType(encoded.data(), encoded.size());
    Require(parsed.IsOk() && parsed.Value().value == 0x2a
        && parsed.Value().encodedBytes == 2
        && parsed.Value().kind == QuicFrameType::Unknown,
        "QUIC frame type parser should preserve unknown extensions");
    Require(!ParseQuicFrameType(nullptr, 0).IsOk(),
        "QUIC frame type parser should reject empty input");
}
