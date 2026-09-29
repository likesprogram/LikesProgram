#include <LikesProgram/Quic/QuicHandshakeDoneFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace LikesProgram {
    namespace Quic {
        Result<QuicHandshakeDoneFrame> ParseQuicHandshakeDoneFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC HANDSHAKE_DONE frame is empty");
            }
            const auto type = ParseQuicVarInt(data, size);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().value != 0x1E) {
                return Status::InvalidArgument(u"QUIC frame is not HANDSHAKE_DONE");
            }
            return QuicHandshakeDoneFrame{ type.Value().encodedBytes };
        }

        Result<std::vector<std::uint8_t>> BuildQuicHandshakeDoneFrame() {
            const auto type = EncodeQuicVarInt(0x1E);
            if (!type.IsOk()) return type.GetStatus();
            return std::vector<std::uint8_t>(
                type.Value().storage.begin(),
                type.Value().storage.begin() + type.Value().size);
        }
    }
}
