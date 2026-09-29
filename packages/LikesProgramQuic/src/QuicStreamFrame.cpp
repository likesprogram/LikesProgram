#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <limits>

namespace {
    LikesProgram::Result<LikesProgram::Quic::QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC STREAM offset is invalid");
        }
        const auto value = LikesProgram::Quic::ParseQuicVarInt(
            data + offset, size - offset);
        if (!value.IsOk()) return value.GetStatus();
        offset += value.Value().encodedBytes;
        return value;
    }

    bool AppendVarInt(
        std::vector<std::uint8_t>& output,
        std::uint64_t value) {
        const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(value);
        if (!encoded.IsOk()) return false;
        output.insert(output.end(), encoded.Value().storage.begin(),
            encoded.Value().storage.begin() + encoded.Value().size);
        return true;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicStreamFrame> ParseQuicStreamFrame(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC STREAM frame is empty");
            }

            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value < 0x08 || type.Value().value > 0x0F) {
                return Status::InvalidArgument(u"QUIC frame is not STREAM");
            }

            QuicStreamFrame frame;
            frame.hasOffset = (type.Value().value & 0x04) != 0;
            frame.hasLength = (type.Value().value & 0x02) != 0;
            frame.fin = (type.Value().value & 0x01) != 0;
            const auto streamId = ReadVarInt(data, size, offset);
            if (!streamId.IsOk()) return streamId.GetStatus();
            frame.streamId = streamId.Value().value;
            if (frame.hasOffset) {
                const auto streamOffset = ReadVarInt(data, size, offset);
                if (!streamOffset.IsOk()) return streamOffset.GetStatus();
                frame.offset = streamOffset.Value().value;
            }

            std::size_t payloadSize = size - offset;
            if (frame.hasLength) {
                const auto length = ReadVarInt(data, size, offset);
                if (!length.IsOk()) return length.GetStatus();
                if (length.Value().value > std::numeric_limits<std::size_t>::max()
                    || length.Value().value > size - offset) {
                    return Status::InvalidArgument(u"QUIC STREAM length exceeds input");
                }
                payloadSize = static_cast<std::size_t>(length.Value().value);
            }
            frame.data = { data + offset, payloadSize };
            frame.consumedBytes = offset + payloadSize;
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicStreamFrame(
            const QuicStreamFrameBuildOptions& options) {
            if (options.streamId > kQuicVarIntMaximum
                || options.offset > kQuicVarIntMaximum
                || options.data.size() > kQuicVarIntMaximum
                || (!options.hasOffset && options.offset != 0)) {
                return Status::InvalidArgument(u"QUIC STREAM options are invalid");
            }

            const auto type = static_cast<std::uint64_t>(0x08)
                | (options.hasOffset ? 0x04 : 0)
                | (options.hasLength ? 0x02 : 0)
                | (options.fin ? 0x01 : 0);
            std::vector<std::uint8_t> output;
            output.reserve(1 + 8 * 3 + options.data.size());
            if (!AppendVarInt(output, type)
                || !AppendVarInt(output, options.streamId)
                || (options.hasOffset && !AppendVarInt(output, options.offset))
                || (options.hasLength && !AppendVarInt(output, options.data.size()))) {
                return Status::InvalidArgument(u"QUIC STREAM value cannot be encoded");
            }
            output.insert(output.end(), options.data.begin(), options.data.end());
            return output;
        }
    }
}
