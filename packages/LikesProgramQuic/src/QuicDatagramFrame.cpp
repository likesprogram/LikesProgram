#include <LikesProgram/Quic/QuicDatagramFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicDatagramFrame> ParseQuicDatagramFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC DATAGRAM frame is empty");
            }
            const auto type = ParseQuicVarInt(data, size);
            if (!type.IsOk()) return type.GetStatus();
            const bool hasLength = type.Value().value == 0x31;
            if (!hasLength && type.Value().value != 0x30) {
                return Status::InvalidArgument(u"QUIC frame is not DATAGRAM");
            }
            std::size_t offset = type.Value().encodedBytes;
            std::size_t payloadSize = size - offset;
            if (hasLength) {
                const auto length = ParseQuicVarInt(data + offset, size - offset);
                if (!length.IsOk()) return length.GetStatus();
                offset += length.Value().encodedBytes;
                if (length.Value().value > size - offset) {
                    return Status::InvalidArgument(u"QUIC DATAGRAM payload is truncated");
                }
                payloadSize = static_cast<std::size_t>(length.Value().value);
            }
            return QuicDatagramFrame{
                hasLength,
                std::span<const std::uint8_t>(data + offset, payloadSize),
                offset + payloadSize };
        }

        Result<std::vector<std::uint8_t>> BuildQuicDatagramFrame(
            bool hasLength,
            std::span<const std::uint8_t> payload) {
            if (payload.size() > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC DATAGRAM payload is too large");
            }
            const auto type = EncodeQuicVarInt(hasLength ? 0x31 : 0x30);
            if (!type.IsOk()) return type.GetStatus();
            std::vector<std::uint8_t> output;
            output.reserve(type.Value().size + payload.size() + (hasLength ? 8 : 0));
            output.insert(output.end(), type.Value().storage.begin(),
                type.Value().storage.begin() + type.Value().size);
            if (hasLength) {
                const auto length = EncodeQuicVarInt(payload.size());
                if (!length.IsOk()) return length.GetStatus();
                output.insert(output.end(), length.Value().storage.begin(),
                    length.Value().storage.begin() + length.Value().size);
            }
            output.insert(output.end(), payload.begin(), payload.end());
            return output;
        }
    }
}
