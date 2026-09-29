#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace {
    using LikesProgram::Quic::QuicFlowControlFrameKind;
    using LikesProgram::Quic::QuicVarIntValue;

    LikesProgram::Result<QuicVarIntValue> ReadVarInt(
        const std::uint8_t* data, std::size_t size, std::size_t& offset) {
        if (offset > size) {
            return LikesProgram::Status::InvalidArgument(
                u"QUIC flow-control offset is invalid");
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

    bool IsKnownType(std::uint64_t type) noexcept {
        return type >= 0x10 && type <= 0x17;
    }

    QuicFlowControlFrameKind KindFromType(std::uint64_t type) noexcept {
        switch (type) {
        case 0x10: return QuicFlowControlFrameKind::MaxData;
        case 0x11: return QuicFlowControlFrameKind::MaxStreamData;
        case 0x12: return QuicFlowControlFrameKind::MaxStreamsBidi;
        case 0x13: return QuicFlowControlFrameKind::MaxStreamsUni;
        case 0x14: return QuicFlowControlFrameKind::DataBlocked;
        case 0x15: return QuicFlowControlFrameKind::StreamDataBlocked;
        case 0x16: return QuicFlowControlFrameKind::StreamsBlockedBidi;
        default: return QuicFlowControlFrameKind::StreamsBlockedUni;
        }
    }

    std::uint64_t TypeFromKind(QuicFlowControlFrameKind kind) noexcept {
        switch (kind) {
        case QuicFlowControlFrameKind::MaxData: return 0x10;
        case QuicFlowControlFrameKind::MaxStreamData: return 0x11;
        case QuicFlowControlFrameKind::MaxStreamsBidi: return 0x12;
        case QuicFlowControlFrameKind::MaxStreamsUni: return 0x13;
        case QuicFlowControlFrameKind::DataBlocked: return 0x14;
        case QuicFlowControlFrameKind::StreamDataBlocked: return 0x15;
        case QuicFlowControlFrameKind::StreamsBlockedBidi: return 0x16;
        case QuicFlowControlFrameKind::StreamsBlockedUni: return 0x17;
        }
        return 0;
    }

    bool HasStreamId(QuicFlowControlFrameKind kind) noexcept {
        return kind == QuicFlowControlFrameKind::MaxStreamData
            || kind == QuicFlowControlFrameKind::StreamDataBlocked;
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicFlowControlFrame> ParseQuicFlowControlFrame(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC flow-control frame is empty");
            }
            std::size_t offset = 0;
            const auto type = ReadVarInt(data, size, offset);
            if (!type.IsOk()) return type.GetStatus();
            if (!IsKnownType(type.Value().value)) {
                return Status::InvalidArgument(u"QUIC frame is not flow-control");
            }

            const auto kind = KindFromType(type.Value().value);
            std::uint64_t streamId = 0;
            if (HasStreamId(kind)) {
                const auto stream = ReadVarInt(data, size, offset);
                if (!stream.IsOk()) return stream.GetStatus();
                streamId = stream.Value().value;
            }
            const auto limit = ReadVarInt(data, size, offset);
            if (!limit.IsOk()) return limit.GetStatus();
            return QuicFlowControlFrame{ kind, streamId, limit.Value().value, offset };
        }

        Result<std::vector<std::uint8_t>> BuildQuicFlowControlFrame(
            const QuicFlowControlFrame& frame) {
            const auto type = TypeFromKind(frame.kind);
            if (!IsKnownType(type) || !IsValueValid(frame.streamId)
                || !IsValueValid(frame.limit)) {
                return Status::InvalidArgument(u"QUIC flow-control values are invalid");
            }
            std::vector<std::uint8_t> output;
            output.reserve(24);
            if (!AppendVarInt(output, type)) return Status::InvalidArgument(
                u"QUIC flow-control type cannot be encoded");
            if (HasStreamId(frame.kind) && !AppendVarInt(output, frame.streamId)) {
                return Status::InvalidArgument(u"QUIC flow-control stream id cannot be encoded");
            }
            if (!AppendVarInt(output, frame.limit)) {
                return Status::InvalidArgument(u"QUIC flow-control limit cannot be encoded");
            }
            return output;
        }
    }
}
