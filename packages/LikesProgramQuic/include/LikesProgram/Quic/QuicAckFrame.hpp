#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicAckRange {
            std::uint64_t smallest = 0;
            std::uint64_t largest = 0;

            friend bool operator==(const QuicAckRange&, const QuicAckRange&) = default;
        };

        struct QuicAckFrame {
            bool ecn = false;
            std::uint64_t largestAcknowledged = 0;
            std::uint64_t ackDelay = 0;
            std::vector<QuicAckRange> ranges{};
            std::uint64_t ect0Count = 0;
            std::uint64_t ect1Count = 0;
            std::uint64_t ecnCeCount = 0;
            std::size_t consumedBytes = 0;

            friend bool operator==(const QuicAckFrame&, const QuicAckFrame&) = default;
        };

        // Parses one ACK or ACK_ECN frame and leaves trailing input untouched.
        // The returned ranges are inclusive and ordered from largest to smallest.
        LIKESPROGRAM_QUIC_API Result<QuicAckFrame> ParseQuicAckFrame(
            const std::uint8_t* data,
            std::size_t size);

        // Builds one ACK or ACK_ECN frame. Packet-number recovery and ACK state
        // management remain outside this wire-format helper.
        LIKESPROGRAM_QUIC_API Result<std::vector<std::uint8_t>>
            BuildQuicAckFrame(const QuicAckFrame& frame);
    }
}
