#include <LikesProgram/Quic/QuicNewTokenFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <limits>

namespace {
    LikesProgram::Result<LikesProgram::Quic::QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(u"QUIC NEW_TOKEN offset is invalid");
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
        Result<QuicNewTokenFrame> ParseQuicNewTokenFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC NEW_TOKEN frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x07) {
                return Status::InvalidArgument(u"QUIC frame is not NEW_TOKEN");
            }
            const auto tokenLength = ReadVarInt(data, size, offset);
            if (!tokenLength.IsOk()) return tokenLength.GetStatus();
            if (tokenLength.Value().value > std::numeric_limits<std::size_t>::max()
                || tokenLength.Value().value > size - offset) {
                return Status::InvalidArgument(u"QUIC NEW_TOKEN length exceeds input");
            }
            QuicNewTokenFrame frame;
            frame.token = { data + offset,
                static_cast<std::size_t>(tokenLength.Value().value) };
            frame.consumedBytes = offset + frame.token.size();
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicNewTokenFrame(
            const QuicNewTokenFrameBuildOptions& options) {
            if (options.token.size() > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC NEW_TOKEN token is too large");
            }
            std::vector<std::uint8_t> output;
            output.reserve(1 + 8 + options.token.size());
            if (!AppendVarInt(output, 0x07)
                || !AppendVarInt(output, options.token.size())) {
                return Status::InvalidArgument(u"QUIC NEW_TOKEN value cannot be encoded");
            }
            output.insert(output.end(), options.token.begin(), options.token.end());
            return output;
        }
    }
}
