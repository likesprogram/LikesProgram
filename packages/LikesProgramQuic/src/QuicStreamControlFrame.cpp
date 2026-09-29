#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace {
    LikesProgram::Result<LikesProgram::Quic::QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data, std::size_t size, std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC stream-control offset is invalid");
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

    bool IsValueValid(std::uint64_t value) noexcept {
        return value <= LikesProgram::Quic::kQuicVarIntMaximum;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicResetStreamFrame> ParseQuicResetStreamFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC RESET_STREAM frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x04) {
                return Status::InvalidArgument(u"QUIC frame is not RESET_STREAM");
            }
            const auto streamId = ReadVarInt(data, size, offset);
            if (!streamId.IsOk()) return streamId.GetStatus();
            const auto errorCode = ReadVarInt(data, size, offset);
            if (!errorCode.IsOk()) return errorCode.GetStatus();
            const auto finalSize = ReadVarInt(data, size, offset);
            if (!finalSize.IsOk()) return finalSize.GetStatus();
            return QuicResetStreamFrame{
                streamId.Value().value, errorCode.Value().value,
                finalSize.Value().value, offset };
        }

        Result<std::vector<std::uint8_t>> BuildQuicResetStreamFrame(
            const QuicResetStreamFrame& frame) {
            if (!IsValueValid(frame.streamId)
                || !IsValueValid(frame.applicationErrorCode)
                || !IsValueValid(frame.finalSize)) {
                return Status::InvalidArgument(u"QUIC RESET_STREAM values are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(32);
            if (!AppendVarInt(output, 0x04)
                || !AppendVarInt(output, frame.streamId)
                || !AppendVarInt(output, frame.applicationErrorCode)
                || !AppendVarInt(output, frame.finalSize)) {
                return Status::InvalidArgument(u"QUIC RESET_STREAM cannot be encoded");
            }
            return output;
        }

        Result<QuicStopSendingFrame> ParseQuicStopSendingFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC STOP_SENDING frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x05) {
                return Status::InvalidArgument(u"QUIC frame is not STOP_SENDING");
            }
            const auto streamId = ReadVarInt(data, size, offset);
            if (!streamId.IsOk()) return streamId.GetStatus();
            const auto errorCode = ReadVarInt(data, size, offset);
            if (!errorCode.IsOk()) return errorCode.GetStatus();
            return QuicStopSendingFrame{
                streamId.Value().value, errorCode.Value().value, offset };
        }

        Result<std::vector<std::uint8_t>> BuildQuicStopSendingFrame(
            const QuicStopSendingFrame& frame) {
            if (!IsValueValid(frame.streamId)
                || !IsValueValid(frame.applicationErrorCode)) {
                return Status::InvalidArgument(u"QUIC STOP_SENDING values are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(24);
            if (!AppendVarInt(output, 0x05)
                || !AppendVarInt(output, frame.streamId)
                || !AppendVarInt(output, frame.applicationErrorCode)) {
                return Status::InvalidArgument(u"QUIC STOP_SENDING cannot be encoded");
            }
            return output;
        }
    }
}
