#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Quic {
        struct QuicCongestionBudgetLimits {
            std::size_t maxWindowBytes = 12000;
            std::size_t maxReservations = 1024;
        };

        struct QuicCongestionBudgetSnapshot {
            std::size_t windowBytes = 0;
            std::size_t inFlightBytes = 0;
            std::size_t availableBytes = 0;
            std::size_t reservations = 0;
        };

        // Caller-owned accounting for bytes admitted to the QUIC send path.
        // It does not choose a congestion algorithm or read a clock.
        class QuicCongestionBudget {
        public:
            LIKESPROGRAM_QUIC_API explicit QuicCongestionBudget(
                QuicCongestionBudgetLimits limits = {}) noexcept;
            LIKESPROGRAM_QUIC_API ~QuicCongestionBudget();
            LIKESPROGRAM_QUIC_API QuicCongestionBudget(QuicCongestionBudget&&) noexcept;
            LIKESPROGRAM_QUIC_API QuicCongestionBudget& operator=(
                QuicCongestionBudget&&) noexcept;
            QuicCongestionBudget(const QuicCongestionBudget&) = delete;
            QuicCongestionBudget& operator=(const QuicCongestionBudget&) = delete;

            LIKESPROGRAM_QUIC_API Result<void> SetWindow(
                std::size_t windowBytes) noexcept;
            LIKESPROGRAM_QUIC_API Result<void> Reserve(
                std::uint64_t packetNumber,
                std::size_t bytes);
            LIKESPROGRAM_QUIC_API Result<void> Acknowledge(
                std::uint64_t packetNumber) noexcept;
            LIKESPROGRAM_QUIC_API Result<void> Lose(
                std::uint64_t packetNumber) noexcept;
            LIKESPROGRAM_QUIC_API QuicCongestionBudgetLimits Limits() const noexcept;
            LIKESPROGRAM_QUIC_API QuicCongestionBudgetSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
