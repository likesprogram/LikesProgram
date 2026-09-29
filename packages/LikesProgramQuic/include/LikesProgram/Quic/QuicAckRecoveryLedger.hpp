#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicAckApplication.hpp>
#include <LikesProgram/Quic/QuicAckTracker.hpp>
#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Quic {
        struct QuicAckRecoveryLimits {
            QuicRetransmissionLimits retransmission{};
            QuicAckApplicationLimits acknowledgement{};
        };

        struct QuicAckRecoverySnapshot {
            QuicAckObservationState acknowledgement{};
            QuicRetransmissionSnapshot retransmission{};
        };

        struct QuicAckRecoveryResult {
            QuicAckObservationResult observation{};
            QuicAckApplicationResult application{};
            QuicAckRecoverySnapshot snapshot{};
        };

        // Caller-owned ACK/loss/recovery accounting. It does not parse protected
        // packets, read a clock, schedule retransmission, or own transport I/O.
        class QuicAckRecoveryLedger final {
        public:
            LIKESPROGRAM_QUIC_API explicit QuicAckRecoveryLedger(
                QuicAckRecoveryLimits limits = {}) noexcept;
            LIKESPROGRAM_QUIC_API ~QuicAckRecoveryLedger();

            LIKESPROGRAM_QUIC_API QuicAckRecoveryLedger(
                QuicAckRecoveryLedger&&) noexcept;
            LIKESPROGRAM_QUIC_API QuicAckRecoveryLedger& operator=(
                QuicAckRecoveryLedger&&) noexcept;
            QuicAckRecoveryLedger(const QuicAckRecoveryLedger&) = delete;
            QuicAckRecoveryLedger& operator=(const QuicAckRecoveryLedger&) = delete;

            LIKESPROGRAM_QUIC_API Result<void> TrackSent(
                std::uint64_t packetNumber,
                const std::vector<std::uint8_t>& payload,
                Time::SteadyTimePoint expiry);

            LIKESPROGRAM_QUIC_API Result<QuicAckRecoveryResult> ApplyAck(
                const QuicAckFrame& frame);
            LIKESPROGRAM_QUIC_API Result<QuicAckRecoveryResult> ApplyAck(
                const QuicAckFrame& frame,
                const QuicAckDelayContext& delayContext);

            LIKESPROGRAM_QUIC_API Result<std::vector<QuicRetransmissionPacket>>
                CollectExpired(Time::SteadyTimePoint now);
            LIKESPROGRAM_QUIC_API QuicAckRecoveryLimits Limits() const noexcept;
            LIKESPROGRAM_QUIC_API QuicAckRecoverySnapshot Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            Result<QuicAckRecoveryResult> ApplyAckInternal(
                const QuicAckFrame& frame,
                const QuicAckDelayContext* delayContext);

            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
