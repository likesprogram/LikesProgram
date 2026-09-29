#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicCongestionBudget.hpp>
#include <LikesProgram/Quic/QuicConnectionCredit.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamCredit.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Quic {
        struct QuicCreditCongestionLedgerLimits {
            QuicCongestionBudgetLimits congestion{};
            std::size_t maxTrackedStreams = 1024;
            std::size_t maxReservations = 1024;
        };

        struct QuicCreditCongestionLedgerSnapshot {
            QuicCongestionBudgetSnapshot congestion{};
            QuicConnectionCreditState connection{};
            std::size_t trackedStreams = 0;
            std::size_t reservations = 0;
        };

        // Caller-owned flow/congestion accounting. It accepts already parsed
        // MAX_DATA/MAX_STREAM_DATA frames and never schedules or emits frames.
        class QuicCreditCongestionLedger final {
        public:
            LIKESPROGRAM_QUIC_API explicit QuicCreditCongestionLedger(
                QuicCreditCongestionLedgerLimits limits = {}) noexcept;
            LIKESPROGRAM_QUIC_API ~QuicCreditCongestionLedger();

            LIKESPROGRAM_QUIC_API QuicCreditCongestionLedger(
                QuicCreditCongestionLedger&&) noexcept;
            LIKESPROGRAM_QUIC_API QuicCreditCongestionLedger& operator=(
                QuicCreditCongestionLedger&&) noexcept;
            QuicCreditCongestionLedger(const QuicCreditCongestionLedger&) = delete;
            QuicCreditCongestionLedger& operator=(
                const QuicCreditCongestionLedger&) = delete;

            LIKESPROGRAM_QUIC_API Result<void> SetCongestionWindow(
                std::size_t windowBytes) noexcept;
            LIKESPROGRAM_QUIC_API Result<void> ApplyPeerFlowControl(
                const QuicFlowControlFrame& frame);
            LIKESPROGRAM_QUIC_API Result<void> Reserve(
                std::uint64_t packetNumber,
                std::uint64_t streamId,
                std::size_t bytes);
            LIKESPROGRAM_QUIC_API Result<void> Acknowledge(
                std::uint64_t packetNumber) noexcept;
            LIKESPROGRAM_QUIC_API Result<void> Lose(
                std::uint64_t packetNumber) noexcept;
            LIKESPROGRAM_QUIC_API Result<QuicStreamCreditState> StreamCredit(
                std::uint64_t streamId) const noexcept;
            LIKESPROGRAM_QUIC_API QuicCreditCongestionLedgerLimits
                Limits() const noexcept;
            LIKESPROGRAM_QUIC_API QuicCreditCongestionLedgerSnapshot
                Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
