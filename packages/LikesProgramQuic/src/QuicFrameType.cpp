#include <LikesProgram/Quic/QuicFrameType.hpp>

namespace LikesProgram {
    namespace Quic {
        QuicFrameType ClassifyQuicFrameType(std::uint64_t value) noexcept {
            if (value == 0x00) return QuicFrameType::Padding;
            if (value == 0x01) return QuicFrameType::Ping;
            if (value == 0x02 || value == 0x03) return QuicFrameType::Ack;
            if (value == 0x04) return QuicFrameType::ResetStream;
            if (value == 0x05) return QuicFrameType::StopSending;
            if (value == 0x06) return QuicFrameType::Crypto;
            if (value == 0x07) return QuicFrameType::NewToken;
            if (value >= 0x08 && value <= 0x0f) return QuicFrameType::Stream;
            if (value == 0x10) return QuicFrameType::MaxData;
            if (value == 0x11) return QuicFrameType::MaxStreamData;
            if (value == 0x12 || value == 0x13) return QuicFrameType::MaxStreams;
            if (value == 0x14) return QuicFrameType::DataBlocked;
            if (value == 0x15) return QuicFrameType::StreamDataBlocked;
            if (value == 0x16 || value == 0x17) return QuicFrameType::StreamsBlocked;
            if (value == 0x18) return QuicFrameType::NewConnectionId;
            if (value == 0x19) return QuicFrameType::RetireConnectionId;
            if (value == 0x1a) return QuicFrameType::PathChallenge;
            if (value == 0x1b) return QuicFrameType::PathResponse;
            if (value == 0x1c || value == 0x1d) return QuicFrameType::ConnectionClose;
            if (value == 0x1e) return QuicFrameType::HandshakeDone;
            if (value == 0x30 || value == 0x31) return QuicFrameType::Datagram;
            return QuicFrameType::Unknown;
        }

        Result<QuicFrameTypeView> ParseQuicFrameType(
            const std::uint8_t* data,
            std::size_t size) {
            auto parsed = ParseQuicVarInt(data, size);
            if (!parsed.IsOk()) return parsed.GetStatus();
            return QuicFrameTypeView{
                parsed.Value().value,
                parsed.Value().encodedBytes,
                ClassifyQuicFrameType(parsed.Value().value) };
        }
    }
}
