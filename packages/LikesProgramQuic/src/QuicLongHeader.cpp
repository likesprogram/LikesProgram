#include <LikesProgram/Quic/QuicLongHeader.hpp>

#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <algorithm>
#include <limits>

namespace {
    constexpr std::size_t kQuicV1ConnectionIdMaximum = 20;
    constexpr std::size_t kRetryIntegrityTagSize = 16;

    std::uint32_t ReadU32(const std::uint8_t* data) noexcept {
        return (static_cast<std::uint32_t>(data[0]) << 24)
            | (static_cast<std::uint32_t>(data[1]) << 16)
            | (static_cast<std::uint32_t>(data[2]) << 8)
            | static_cast<std::uint32_t>(data[3]);
    }

    void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
        output.push_back(static_cast<std::uint8_t>(value >> 24));
        output.push_back(static_cast<std::uint8_t>(value >> 16));
        output.push_back(static_cast<std::uint8_t>(value >> 8));
        output.push_back(static_cast<std::uint8_t>(value));
    }

    bool IsPayloadPacket(LikesProgram::Quic::QuicLongPacketType type) noexcept {
        return type == LikesProgram::Quic::QuicLongPacketType::Initial
            || type == LikesProgram::Quic::QuicLongPacketType::ZeroRtt
            || type == LikesProgram::Quic::QuicLongPacketType::Handshake;
    }

    bool IsKnownType(LikesProgram::Quic::QuicLongPacketType type) noexcept {
        return IsPayloadPacket(type)
            || type == LikesProgram::Quic::QuicLongPacketType::Retry;
    }

    LikesProgram::Result<std::span<const std::uint8_t>> ReadVarIntSpan(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& offset,
        const char16_t* error) {
        if (offset > size) return LikesProgram::Status::InvalidArgument(error);
        auto parsed = LikesProgram::Quic::ParseQuicVarInt(data + offset, size - offset);
        if (!parsed.IsOk()) return parsed.GetStatus();
        const auto value = parsed.Value();
        if (value.value > std::numeric_limits<std::size_t>::max()
            || value.encodedBytes > size - offset
            || value.value > size - offset - value.encodedBytes) {
            return LikesProgram::Status::InvalidArgument(error);
        }
        offset += value.encodedBytes;
        const auto* begin = data + offset;
        offset += static_cast<std::size_t>(value.value);
        return std::span<const std::uint8_t>(begin, static_cast<std::size_t>(value.value));
    }
}

namespace LikesProgram {
    namespace Quic {
        Result<QuicLongHeaderView> ParseQuicLongHeader(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size < 6) {
                return Status::InvalidArgument(u"QUIC long header is incomplete");
            }
            if ((data[0] & 0x80) == 0) {
                return Status::InvalidArgument(u"QUIC packet is not a long header");
            }

            QuicLongHeaderView view;
            view.version = ReadU32(data + 1);
            const bool versionNegotiation = view.version == 0;
            if (!versionNegotiation && (data[0] & 0x40) == 0) {
                return Status::InvalidArgument(u"QUIC long header fixed bit is clear");
            }

            const std::size_t connectionIdMaximum = versionNegotiation
                ? 255 : kQuicV1ConnectionIdMaximum;
            std::size_t offset = 5;
            const std::size_t destinationLength = data[offset++];
            if (destinationLength > connectionIdMaximum
                || destinationLength > size - offset) {
                return Status::InvalidArgument(u"QUIC destination connection id is invalid");
            }
            view.destinationConnectionId = {
                data + offset, destinationLength };
            offset += destinationLength;
            if (offset >= size) {
                return Status::InvalidArgument(u"QUIC source connection id length is missing");
            }
            const std::size_t sourceLength = data[offset++];
            if (sourceLength > connectionIdMaximum
                || sourceLength > size - offset) {
                return Status::InvalidArgument(u"QUIC source connection id is invalid");
            }
            view.sourceConnectionId = { data + offset, sourceLength };
            offset += sourceLength;

            if (versionNegotiation) {
                view.type = QuicLongPacketType::VersionNegotiation;
                view.versionNegotiationVersions = {
                    data + offset, size - offset };
                if (view.versionNegotiationVersions.size() == 0
                    || view.versionNegotiationVersions.size() % 4 != 0) {
                    return Status::InvalidArgument(
                        u"QUIC version negotiation list is incomplete");
                }
                view.consumedBytes = size;
                return view;
            }

            view.type = static_cast<QuicLongPacketType>((data[0] >> 4) & 0x03);
            if (!IsKnownType(view.type)) {
                return Status::InvalidArgument(u"QUIC long packet type is invalid");
            }
            if (view.type == QuicLongPacketType::Retry) {
                if (size - offset < kRetryIntegrityTagSize) {
                    return Status::InvalidArgument(
                        u"QUIC Retry integrity tag is incomplete");
                }
                const std::size_t tokenSize = size - offset - kRetryIntegrityTagSize;
                view.token = { data + offset, tokenSize };
                view.retryIntegrityTag = {
                    data + offset + tokenSize, kRetryIntegrityTagSize };
                view.consumedBytes = size;
                return view;
            }

            if (view.type == QuicLongPacketType::Initial) {
                auto token = ReadVarIntSpan(
                    data, size, offset, u"QUIC Initial token length is invalid");
                if (!token.IsOk()) return token.GetStatus();
                view.token = token.Value();
            }

            auto length = ParseQuicVarInt(data + offset, size - offset);
            if (!length.IsOk()) return length.GetStatus();
            offset += length.Value().encodedBytes;
            const std::size_t packetLength = static_cast<std::size_t>(length.Value().value);
            if (length.Value().value > std::numeric_limits<std::size_t>::max()
                || packetLength > size - offset) {
                return Status::InvalidArgument(u"QUIC packet length exceeds datagram");
            }
            view.packetNumberLength = static_cast<std::size_t>((data[0] & 0x03) + 1);
            view.packetNumberOffset = offset;
            if (packetLength < view.packetNumberLength) {
                return Status::InvalidArgument(u"QUIC packet length omits packet number");
            }
            view.packetNumber = { data + offset, view.packetNumberLength };
            view.payload = {
                data + offset + view.packetNumberLength,
                packetLength - view.packetNumberLength };
            view.consumedBytes = offset + packetLength;
            return view;
        }

        Result<std::vector<std::uint8_t>> BuildQuicLongHeaderPacket(
            const QuicLongHeaderBuildOptions& options) {
            if (options.version == 0
                || !IsKnownType(options.type)
                || options.destinationConnectionId.size() > kQuicV1ConnectionIdMaximum
                || options.sourceConnectionId.size() > kQuicV1ConnectionIdMaximum) {
                return Status::InvalidArgument(u"QUIC long header options are invalid");
            }
            if (options.type == QuicLongPacketType::Retry) {
                if (options.token.empty()
                    || !options.packetNumber.empty()
                    || !options.payload.empty()
                    || options.retryIntegrityTag.size() != kRetryIntegrityTagSize) {
                    return Status::InvalidArgument(u"QUIC Retry options are invalid");
                }
            } else {
                if (options.packetNumber.empty() || options.packetNumber.size() > 4
                    || (options.type != QuicLongPacketType::Initial
                        && !options.token.empty())) {
                    return Status::InvalidArgument(u"QUIC packet number or token is invalid");
                }
            }

            if (options.payload.size() >
                std::numeric_limits<std::size_t>::max() - options.packetNumber.size()) {
                return Status::InvalidArgument(u"QUIC packet payload size overflows");
            }
            const std::size_t packetLength =
                options.packetNumber.size() + options.payload.size();
            if (packetLength > kQuicVarIntMaximum) {
                return Status::InvalidArgument(u"QUIC packet payload is too large");
            }
            const auto length = EncodeQuicVarInt(packetLength);
            if (!length.IsOk()) return length.GetStatus();

            std::vector<std::uint8_t> output;
            output.reserve(1 + 4 + 1 + options.destinationConnectionId.size()
                + 1 + options.sourceConnectionId.size() + 8 + options.token.size()
                + length.Value().size + options.packetNumber.size()
                + options.payload.size() + options.retryIntegrityTag.size());
            const std::uint8_t typeBits =
                static_cast<std::uint8_t>(options.type) << 4;
            const std::uint8_t packetNumberLength = options.packetNumber.empty()
                ? 0 : static_cast<std::uint8_t>(options.packetNumber.size() - 1);
            output.push_back(static_cast<std::uint8_t>(0xC0 | typeBits
                | packetNumberLength));
            AppendU32(output, options.version);
            output.push_back(static_cast<std::uint8_t>(options.destinationConnectionId.size()));
            output.insert(output.end(), options.destinationConnectionId.begin(),
                options.destinationConnectionId.end());
            output.push_back(static_cast<std::uint8_t>(options.sourceConnectionId.size()));
            output.insert(output.end(), options.sourceConnectionId.begin(),
                options.sourceConnectionId.end());

            if (options.type == QuicLongPacketType::Initial) {
                const auto tokenLength = EncodeQuicVarInt(options.token.size());
                if (!tokenLength.IsOk()) return tokenLength.GetStatus();
                output.insert(output.end(), tokenLength.Value().storage.begin(),
                    tokenLength.Value().storage.begin() + tokenLength.Value().size);
                output.insert(output.end(), options.token.begin(), options.token.end());
            }
            if (options.type == QuicLongPacketType::Retry) {
                output.insert(output.end(), options.token.begin(), options.token.end());
                output.insert(output.end(), options.retryIntegrityTag.begin(),
                    options.retryIntegrityTag.end());
                return output;
            }

            output.insert(output.end(), length.Value().storage.begin(),
                length.Value().storage.begin() + length.Value().size);
            output.insert(output.end(), options.packetNumber.begin(),
                options.packetNumber.end());
            output.insert(output.end(), options.payload.begin(), options.payload.end());
            return output;
        }
    }
}
