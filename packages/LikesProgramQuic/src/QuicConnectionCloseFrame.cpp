#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <limits>

namespace {
    LikesProgram::Result<LikesProgram::Quic::QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data, std::size_t size, std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC close offset is invalid");
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
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicConnectionCloseFrame> ParseQuicConnectionCloseFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC CONNECTION_CLOSE frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x1C && type.Value().value != 0x1D) {
                return Status::InvalidArgument(u"QUIC frame is not CONNECTION_CLOSE");
            }
            const auto errorCode = ReadVarInt(data, size, offset);
            if (!errorCode.IsOk()) return errorCode.GetStatus();
            QuicConnectionCloseFrame frame;
            frame.application = type.Value().value == 0x1D;
            frame.errorCode = errorCode.Value().value;
            if (!frame.application) {
                const auto frameType = ReadVarInt(data, size, offset);
                if (!frameType.IsOk()) return frameType.GetStatus();
                frame.frameType = frameType.Value().value;
            }
            const auto reasonLength = ReadVarInt(data, size, offset);
            if (!reasonLength.IsOk()) return reasonLength.GetStatus();
            if (reasonLength.Value().value > std::numeric_limits<std::size_t>::max()
                || reasonLength.Value().value > size - offset) {
                return Status::InvalidArgument(u"QUIC close reason exceeds input");
            }
            frame.reason = { data + offset,
                static_cast<std::size_t>(reasonLength.Value().value) };
            frame.consumedBytes = offset + frame.reason.size();
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicConnectionCloseFrame(
            const QuicConnectionCloseFrame& frame) {
            if (frame.errorCode > kQuicVarIntMaximum
                || frame.frameType > kQuicVarIntMaximum
                || frame.reason.size() > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC close values are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(1 + 32 + frame.reason.size());
            if (!AppendVarInt(output, frame.application ? 0x1D : 0x1C)
                || !AppendVarInt(output, frame.errorCode)
                || (!frame.application && !AppendVarInt(output, frame.frameType))
                || !AppendVarInt(output, frame.reason.size())) {
                return Status::InvalidArgument(u"QUIC close value cannot be encoded");
            }
            output.insert(output.end(), frame.reason.begin(), frame.reason.end());
            return output;
        }
    }
}
