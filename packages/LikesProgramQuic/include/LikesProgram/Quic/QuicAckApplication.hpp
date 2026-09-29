#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Quic {
        struct QuicAckApplicationLimits {
            std::uint64_t maxPacketNumbers = 4096;
        };

        struct QuicAckApplicationResult {
            std::uint64_t acknowledgedPackets = 0;
            std::uint64_t untrackedPackets = 0;
        };

        // Applies validated ACK ranges to caller-owned retransmission bookkeeping.
        // The bounded traversal deliberately excludes loss policy, timers and I/O.
        LIKESPROGRAM_QUIC_API Result<QuicAckApplicationResult>
            ApplyQuicAckFrame(
                QuicRetransmissionQueue& queue,
                const QuicAckFrame& frame,
                QuicAckApplicationLimits limits = {});
    }
}
