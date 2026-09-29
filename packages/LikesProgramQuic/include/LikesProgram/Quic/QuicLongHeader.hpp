#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        enum class QuicLongPacketType : std::uint8_t {
            Initial = 0,
            ZeroRtt = 1,
            Handshake = 2,
            Retry = 3,
            VersionNegotiation = 4
        };

        struct QuicLongHeaderView {
            QuicLongPacketType type = QuicLongPacketType::Initial;
            std::uint32_t version = 0;
            std::size_t consumedBytes = 0;
            std::size_t packetNumberLength = 0;
            std::span<const std::uint8_t> destinationConnectionId{};
            std::span<const std::uint8_t> sourceConnectionId{};
            std::span<const std::uint8_t> token{};
            std::span<const std::uint8_t> packetNumber{};
            std::span<const std::uint8_t> payload{};
            std::span<const std::uint8_t> retryIntegrityTag{};
            std::span<const std::uint8_t> versionNegotiationVersions{};
            // Offset of the packet number, independent of the protected
            // packet-number length bits in the first byte.
            std::size_t packetNumberOffset = 0;
        };

        struct QuicLongHeaderBuildOptions {
            QuicLongPacketType type = QuicLongPacketType::Initial;
            std::uint32_t version = 1;
            std::span<const std::uint8_t> destinationConnectionId{};
            std::span<const std::uint8_t> sourceConnectionId{};
            std::span<const std::uint8_t> token{};
            std::span<const std::uint8_t> packetNumber{};
            std::span<const std::uint8_t> payload{};
            std::span<const std::uint8_t> retryIntegrityTag{};
        };

        // Parses one long-header packet and returns spans into the caller buffer.
        // Packet protection and header protection are intentionally outside this API.
        LIKESPROGRAM_QUIC_API Result<QuicLongHeaderView> ParseQuicLongHeader(
            const std::uint8_t* data,
            std::size_t size);

        // Builds Initial, 0-RTT, Handshake and Retry long-header packets with
        // opaque packet-number/payload bytes. Version Negotiation is parse-only.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicLongHeaderPacket(const QuicLongHeaderBuildOptions& options);
    }
}
