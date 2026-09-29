#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicShortHeaderView {
            bool spinBit = false;
            bool keyPhase = false;
            std::size_t packetNumberLength = 0;
            std::size_t consumedBytes = 0;
            std::span<const std::uint8_t> destinationConnectionId{};
            std::span<const std::uint8_t> packetNumber{};
            std::span<const std::uint8_t> payload{};
        };

        struct QuicShortHeaderBuildOptions {
            bool spinBit = false;
            bool keyPhase = false;
            std::span<const std::uint8_t> destinationConnectionId{};
            std::span<const std::uint8_t> packetNumber{};
            std::span<const std::uint8_t> payload{};
        };

        // Parses one already-unprotected QUIC v1 short-header datagram and
        // returns spans into the caller buffer. Header protection and packet
        // number reconstruction remain outside this API.
        LIKESPROGRAM_QUIC_API Result<QuicShortHeaderView> ParseQuicShortHeader(
            const std::uint8_t* data,
            std::size_t size,
            std::size_t destinationConnectionIdLength);

        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicShortHeaderPacket(const QuicShortHeaderBuildOptions& options);
    }
}
