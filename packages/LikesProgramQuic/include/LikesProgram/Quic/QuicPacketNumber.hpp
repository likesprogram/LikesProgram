#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace LikesProgram {
    namespace Quic {
        struct QuicPacketNumberEncoding {
            std::array<std::uint8_t, 4> storage{};
            std::size_t size = 0;

            const std::uint8_t* Data() const noexcept {
                return storage.data();
            }

            friend bool operator==(const QuicPacketNumberEncoding&,
                const QuicPacketNumberEncoding&) = default;
        };

        LIKESPROGRAM_QUIC_API Result<QuicPacketNumberEncoding>
            EncodeQuicPacketNumber(
                std::uint64_t fullPacketNumber,
                std::optional<std::uint64_t> largestAcknowledged = std::nullopt);

        LIKESPROGRAM_QUIC_API Result<std::uint64_t> DecodeQuicPacketNumber(
            std::uint64_t largestReceivedPacketNumber,
            std::uint64_t truncatedPacketNumber,
            std::size_t encodedBytes);
    }
}
