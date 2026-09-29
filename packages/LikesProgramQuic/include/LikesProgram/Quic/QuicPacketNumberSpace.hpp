#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicPacketNumber.hpp>

#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        struct QuicPacketNumberSpaceSnapshot {
            bool hasLargestReceived = false;
            std::uint64_t largestReceived = 0;
        };

        // Caller-owned packet-number reconstruction state for one number space.
        // It never reads a clock and does not own packet protection or I/O.
        class QuicPacketNumberSpace {
        public:
            LIKESPROGRAM_QUIC_API Result<std::uint64_t> Decode(
                std::uint64_t truncatedPacketNumber,
                std::size_t encodedBytes);
            LIKESPROGRAM_QUIC_API Result<void> Observe(
                std::uint64_t fullPacketNumber) noexcept;
            LIKESPROGRAM_QUIC_API QuicPacketNumberSpaceSnapshot
                Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            bool m_hasLargestReceived = false;
            std::uint64_t m_largestReceived = 0;
        };
    }
}
