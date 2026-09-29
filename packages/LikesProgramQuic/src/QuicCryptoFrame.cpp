#include <LikesProgram/Quic/QuicCryptoFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <limits>

namespace {
    LikesProgram::Result<LikesProgram::Quic::QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC CRYPTO offset is invalid");
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
        Result<QuicCryptoFrame> ParseQuicCryptoFrame(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC CRYPTO frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x06) {
                return Status::InvalidArgument(u"QUIC frame is not CRYPTO");
            }
            const auto frameOffset = ReadVarInt(data, size, offset);
            if (!frameOffset.IsOk()) return frameOffset.GetStatus();
            const auto length = ReadVarInt(data, size, offset);
            if (!length.IsOk()) return length.GetStatus();
            if (length.Value().value > std::numeric_limits<std::size_t>::max()
                || length.Value().value > size - offset) {
                return Status::InvalidArgument(u"QUIC CRYPTO length exceeds input");
            }
            QuicCryptoFrame frame;
            frame.offset = frameOffset.Value().value;
            frame.data = { data + offset, static_cast<std::size_t>(length.Value().value) };
            frame.consumedBytes = offset + frame.data.size();
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicCryptoFrame(
            const QuicCryptoFrameBuildOptions& options) {
            if (options.offset > kQuicVarIntMaximum
                || options.data.size() > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC CRYPTO options are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(1 + 16 + options.data.size());
            if (!AppendVarInt(output, 0x06)
                || !AppendVarInt(output, options.offset)
                || !AppendVarInt(output, options.data.size())) {
                return Status::InvalidArgument(u"QUIC CRYPTO value cannot be encoded");
            }
            output.insert(output.end(), options.data.begin(), options.data.end());
            return output;
        }
    }
}
