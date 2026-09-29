#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <limits>

namespace {
    using LikesProgram::Quic::QuicConnectionIdFrameKind;
    using LikesProgram::Quic::QuicVarIntValue;

    LikesProgram::Result<QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data, std::size_t size, std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(
                u"QUIC connection-id offset is invalid");
        }
        const auto value = LikesProgram::Quic::ParseQuicVarInt(
            data + offset, size - offset);
        if (!value.IsOk()) return value.GetStatus();
        offset += value.Value().encodedBytes;
        return value;
    }

    bool AppendVarInt(std::vector<std::uint8_t>& output, std::uint64_t value) {
        const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(value);
        if (!encoded.IsOk()) return false;
        output.insert(output.end(), encoded.Value().storage.begin(),
            encoded.Value().storage.begin() + encoded.Value().size);
        return true;
    }

    std::uint64_t TypeFromKind(QuicConnectionIdFrameKind kind) noexcept {
        switch (kind) {
        case QuicConnectionIdFrameKind::NewConnectionId: return 0x18;
        case QuicConnectionIdFrameKind::RetireConnectionId: return 0x19;
        }
        return 0;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicConnectionIdFrame> ParseQuicConnectionIdFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC connection-id frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            QuicConnectionIdFrame frame;
            if (type.Value().value == 0x19) {
                const auto sequence = ReadVarInt(data, size, offset);
                if (!sequence.IsOk()) return sequence.GetStatus();
                frame.kind = QuicConnectionIdFrameKind::RetireConnectionId;
                frame.sequenceNumber = sequence.Value().value;
                frame.consumedBytes = offset;
                return frame;
            }
            if (type.Value().value != 0x18) {
                return Status::InvalidArgument(u"QUIC frame is not connection-id");
            }
            const auto sequence = ReadVarInt(data, size, offset);
            if (!sequence.IsOk()) return sequence.GetStatus();
            const auto retire = ReadVarInt(data, size, offset);
            if (!retire.IsOk()) return retire.GetStatus();
            if (retire.Value().value > sequence.Value().value) {
                return Status::InvalidArgument(
                    u"QUIC NEW_CONNECTION_ID retire value exceeds sequence");
            }
            if (offset >= size) {
                return Status::InvalidArgument(u"QUIC connection-id length is missing");
            }
            const auto connectionIdLength = static_cast<std::size_t>(data[offset++]);
            if (connectionIdLength == 0 || connectionIdLength > 20
                || size - offset < connectionIdLength + 16) {
                return Status::InvalidArgument(u"QUIC connection-id payload is invalid");
            }
            frame.kind = QuicConnectionIdFrameKind::NewConnectionId;
            frame.sequenceNumber = sequence.Value().value;
            frame.retirePriorTo = retire.Value().value;
            frame.connectionId = { data + offset, connectionIdLength };
            offset += connectionIdLength;
            frame.statelessResetToken = { data + offset, 16 };
            frame.consumedBytes = offset + 16;
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicConnectionIdFrame(
            const QuicConnectionIdFrame& frame) {
            const auto type = TypeFromKind(frame.kind);
            if (type == 0 || frame.sequenceNumber > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC connection-id values are invalid");
            }
            if (frame.kind == QuicConnectionIdFrameKind::RetireConnectionId) {
                std::vector<std::uint8_t> output;
                output.reserve(16);
                if (!AppendVarInt(output, type)
                    || !AppendVarInt(output, frame.sequenceNumber)) {
                    return Status::InvalidArgument(u"QUIC RETIRE_CONNECTION_ID cannot be encoded");
                }
                return output;
            }
            if (frame.retirePriorTo > frame.sequenceNumber
                || frame.retirePriorTo > kQuicVarIntMaximum
                || frame.connectionId.empty() || frame.connectionId.size() > 20
                || frame.statelessResetToken.size() != 16) {
                return Status::InvalidArgument(u"QUIC NEW_CONNECTION_ID values are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(32 + frame.connectionId.size());
            if (!AppendVarInt(output, type)
                || !AppendVarInt(output, frame.sequenceNumber)
                || !AppendVarInt(output, frame.retirePriorTo)) {
                return Status::InvalidArgument(u"QUIC NEW_CONNECTION_ID cannot be encoded");
            }
            output.push_back(static_cast<std::uint8_t>(frame.connectionId.size()));
            output.insert(output.end(), frame.connectionId.begin(), frame.connectionId.end());
            output.insert(output.end(), frame.statelessResetToken.begin(),
                frame.statelessResetToken.end());
            return output;
        }
    }
}
