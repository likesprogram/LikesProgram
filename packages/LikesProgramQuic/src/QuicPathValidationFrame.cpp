#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace {
    using LikesProgram::Quic::QuicPathValidationFrameKind;

    std::uint64_t TypeFromKind(QuicPathValidationFrameKind kind) noexcept {
        switch (kind) {
        case QuicPathValidationFrameKind::PathChallenge: return 0x1a;
        case QuicPathValidationFrameKind::PathResponse: return 0x1b;
        }
        return 0;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicPathValidationFrame> ParseQuicPathValidationFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC path-validation frame is empty");
            }
            const auto type = ParseQuicVarInt(data, size);
            if (!type.IsOk()) return type.GetStatus();
            QuicPathValidationFrameKind kind;
            if (type.Value().value == 0x1a) {
                kind = QuicPathValidationFrameKind::PathChallenge;
            } else if (type.Value().value == 0x1b) {
                kind = QuicPathValidationFrameKind::PathResponse;
            } else {
                return Status::InvalidArgument(u"QUIC frame is not path validation");
            }
            const auto offset = type.Value().encodedBytes;
            if (size - offset < 8) {
                return Status::InvalidArgument(u"QUIC path-validation data is truncated");
            }
            QuicPathValidationFrame frame;
            frame.kind = kind;
            for (std::size_t i = 0; i < frame.data.size(); ++i) {
                frame.data[i] = data[offset + i];
            }
            frame.consumedBytes = offset + frame.data.size();
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildQuicPathValidationFrame(
            const QuicPathValidationFrame& frame) {
            const auto type = TypeFromKind(frame.kind);
            if (type == 0) {
                return Status::InvalidArgument(u"QUIC path-validation kind is invalid");
            }
            const auto encoded = EncodeQuicVarInt(type);
            if (!encoded.IsOk()) return encoded.GetStatus();
            std::vector<std::uint8_t> output;
            output.reserve(encoded.Value().size + frame.data.size());
            output.insert(output.end(), encoded.Value().storage.begin(),
                encoded.Value().storage.begin() + encoded.Value().size);
            output.insert(output.end(), frame.data.begin(), frame.data.end());
            return output;
        }

        Result<QuicPathValidationFrame> BuildQuicPathResponseFrame(
            const QuicPathValidationFrame& challenge) {
            if (challenge.kind != QuicPathValidationFrameKind::PathChallenge) {
                return Status::InvalidArgument(
                    u"QUIC path response requires a PATH_CHALLENGE token");
            }
            QuicPathValidationFrame response;
            response.kind = QuicPathValidationFrameKind::PathResponse;
            response.data = challenge.data;
            return response;
        }
    }
}
