#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/time/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicRetransmissionLimits {
            std::size_t maxPackets = 1024;
            std::size_t maxPayloadBytes = 1024 * 1024;
        };

        struct QuicRetransmissionPacket {
            std::uint64_t packetNumber = 0;
            std::vector<std::uint8_t> payload;
            Time::SteadyTimePoint expiry{};
        };

        struct QuicRetransmissionSnapshot {
            std::size_t trackedPackets = 0;
            std::size_t trackedBytes = 0;
        };

        // Caller-owned loss-recovery bookkeeping for opaque sent packets.
        // It does not read a clock, retransmit bytes, or own a timer/socket.
        class QuicRetransmissionQueue {
        public:
            LIKESPROGRAM_QUIC_API explicit QuicRetransmissionQueue(
                QuicRetransmissionLimits limits = {}) noexcept;
            LIKESPROGRAM_QUIC_API ~QuicRetransmissionQueue();

            LIKESPROGRAM_QUIC_API QuicRetransmissionQueue(
                QuicRetransmissionQueue&&) noexcept;
            LIKESPROGRAM_QUIC_API QuicRetransmissionQueue& operator=(
                QuicRetransmissionQueue&&) noexcept;
            QuicRetransmissionQueue(const QuicRetransmissionQueue&) = delete;
            QuicRetransmissionQueue& operator=(const QuicRetransmissionQueue&) = delete;

            LIKESPROGRAM_QUIC_API Result<void> Track(
                std::uint64_t packetNumber,
                const std::vector<std::uint8_t>& payload,
                Time::SteadyTimePoint expiry);
            LIKESPROGRAM_QUIC_API Result<void> Acknowledge(
                std::uint64_t packetNumber) noexcept;
            LIKESPROGRAM_QUIC_API Result<std::vector<QuicRetransmissionPacket>>
                CollectExpired(
                Time::SteadyTimePoint now);

            LIKESPROGRAM_QUIC_API QuicRetransmissionLimits Limits() const noexcept;
            LIKESPROGRAM_QUIC_API QuicRetransmissionSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
