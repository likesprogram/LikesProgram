#include <LikesProgram/Quic/QuicPingFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicPingFrame> ParseQuicPingFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC PING frame is empty");
            }
            const auto type = ParseQuicVarInt(data, size);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x01) {
                return Status::InvalidArgument(u"QUIC frame is not PING");
            }
            return QuicPingFrame{ type.Value().encodedBytes };
        }

        Result<std::vector<std::uint8_t>> BuildQuicPingFrame() {
            const auto type = EncodeQuicVarInt(0x01);
            if (!type.IsOk()) return type.GetStatus();
            return std::vector<std::uint8_t>(
                type.Value().storage.begin(),
                type.Value().storage.begin() + type.Value().size);
        }
    }
}
