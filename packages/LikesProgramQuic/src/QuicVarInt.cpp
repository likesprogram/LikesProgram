#include <LikesProgram/Quic/QuicVarInt.hpp>

namespace {
    constexpr std::uint64_t LimitForWidth(std::size_t width) noexcept {
        switch (width) {
        case 1: return (std::uint64_t{ 1 } << 6) - 1;
        case 2: return (std::uint64_t{ 1 } << 14) - 1;
        case 4: return (std::uint64_t{ 1 } << 30) - 1;
        case 8: return LikesProgram::Quic::kQuicVarIntMaximum;
        default: return 0;
        }
    }

    constexpr std::uint8_t PrefixForWidth(std::size_t width) noexcept {
        switch (width) {
        case 1: return 0x00;
        case 2: return 0x40;
        case 4: return 0x80;
        case 8: return 0xC0;
        default: return 0;
        }
    }

    constexpr std::size_t WidthValue(
        LikesProgram::Quic::QuicVarIntWidth width) noexcept {
        return static_cast<std::size_t>(width);
    }
}

namespace LikesProgram {
    namespace Quic {
        std::size_t QuicVarIntEncodedSize(std::uint64_t value) noexcept {
            if (value <= LimitForWidth(1)) return 1;
            if (value <= LimitForWidth(2)) return 2;
            if (value <= LimitForWidth(4)) return 4;
            if (value <= LimitForWidth(8)) return 8;
            return 0;
        }

        Result<QuicVarIntValue> ParseQuicVarInt(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"QUIC varint is empty");
            }

            const std::size_t width = std::size_t{ 1 } << (data[0] >> 6);
            if (size < width) {
                return Status::InvalidArgument(u"QUIC varint is incomplete");
            }

            std::uint64_t value = data[0] & 0x3F;
            for (std::size_t index = 1; index < width; ++index) {
                value = (value << 8) | data[index];
            }
            return QuicVarIntValue{ value, width };
        }

        Result<QuicVarIntEncoding> EncodeQuicVarInt(
            std::uint64_t value,
            QuicVarIntWidth requestedWidth) {
            const std::size_t minimumWidth = QuicVarIntEncodedSize(value);
            if (minimumWidth == 0) {
                return Status::InvalidArgument(u"QUIC varint exceeds 62-bit range");
            }

            std::size_t width = WidthValue(requestedWidth);
            if (requestedWidth == QuicVarIntWidth::Minimum) width = minimumWidth;
            if (width != 1 && width != 2 && width != 4 && width != 8) {
                return Status::InvalidArgument(u"QUIC varint width is invalid");
            }
            if (value > LimitForWidth(width)) {
                return Status::InvalidArgument(u"QUIC varint does not fit requested width");
            }

            QuicVarIntEncoding output;
            output.size = width;
            for (std::size_t index = width; index > 0; --index) {
                output.storage[index - 1] = static_cast<std::uint8_t>(value & 0xFF);
                value >>= 8;
            }
            output.storage[0] = static_cast<std::uint8_t>(
                output.storage[0] | PrefixForWidth(width));
            return output;
        }
    }
}
