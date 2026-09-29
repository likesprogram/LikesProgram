#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        inline constexpr std::uint64_t kQuicVarIntMaximum =
            (std::uint64_t{ 1 } << 62) - 1;

        enum class QuicVarIntWidth : std::uint8_t {
            Minimum = 0,
            One = 1,
            Two = 2,
            Four = 4,
            Eight = 8
        };

        struct QuicVarIntValue {
            std::uint64_t value = 0;
            std::size_t encodedBytes = 0;

            friend bool operator==(const QuicVarIntValue&, const QuicVarIntValue&) = default;
        };

        struct QuicVarIntEncoding {
            std::array<std::uint8_t, 8> storage{};
            std::size_t size = 0;

            const std::uint8_t* Data() const noexcept {
                return storage.data();
            }

            std::uint8_t* Data() noexcept {
                return storage.data();
            }

            friend bool operator==(const QuicVarIntEncoding&, const QuicVarIntEncoding&) = default;
        };

        // Returns the shortest RFC 9000 encoding width, or zero when out of range.
        LIKESPROGRAM_QUIC_API std::size_t QuicVarIntEncodedSize(
            std::uint64_t value) noexcept;

        // Parses one variable-length integer and leaves trailing input untouched.
        LIKESPROGRAM_QUIC_API Result<QuicVarIntValue> ParseQuicVarInt(
            const std::uint8_t* data,
            std::size_t size);

        // Minimum selects the shortest encoding. Explicit widths allow RFC 9000
        // non-minimal encodings where the containing field permits them.
        LIKESPROGRAM_QUIC_API Result<QuicVarIntEncoding> EncodeQuicVarInt(
            std::uint64_t value,
            QuicVarIntWidth width = QuicVarIntWidth::Minimum);
    }
}
