#pragma once

#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicConnectionIdTracker.hpp>
#include <LikesProgram/Quic/QuicPathProbeTracker.hpp>
#include <LikesProgram/Quic/QuicPathValidationAction.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <memory>

namespace LikesProgram {
    namespace Quic {
        struct QuicPathMigrationLedgerSnapshot {
            QuicPathProbeState probe{};
            QuicConnectionIdObservationState connectionId{};
        };

        struct QuicPathMigrationChallengeResult {
            QuicPathProbeResult probe{};
            QuicActionMessage response{};
        };

        // Caller-owned path validation/migration accounting. It does not own
        // addresses, timers, UDP I/O, connection-ID policy, or action delivery.
        class QuicPathMigrationLedger final {
        public:
            LIKESPROGRAM_QUIC_API QuicPathMigrationLedger() noexcept;
            LIKESPROGRAM_QUIC_API ~QuicPathMigrationLedger();

            LIKESPROGRAM_QUIC_API QuicPathMigrationLedger(
                QuicPathMigrationLedger&&) noexcept;
            LIKESPROGRAM_QUIC_API QuicPathMigrationLedger& operator=(
                QuicPathMigrationLedger&&) noexcept;
            QuicPathMigrationLedger(const QuicPathMigrationLedger&) = delete;
            QuicPathMigrationLedger& operator=(
                const QuicPathMigrationLedger&) = delete;

            LIKESPROGRAM_QUIC_API Result<QuicPathMigrationChallengeResult>
                ObserveChallenge(const QuicPathValidationFrame& frame);
            LIKESPROGRAM_QUIC_API Result<QuicPathProbeResult>
                BeginValidation(const std::array<std::uint8_t, 8>& token) noexcept;
            LIKESPROGRAM_QUIC_API Result<QuicPathProbeResult>
                ObserveResponse(const QuicPathValidationFrame& frame) noexcept;
            LIKESPROGRAM_QUIC_API void CancelValidation() noexcept;
            LIKESPROGRAM_QUIC_API Result<QuicConnectionIdObservationResult>
                ObserveConnectionId(const QuicConnectionIdFrame& frame) noexcept;
            LIKESPROGRAM_QUIC_API QuicPathMigrationLedgerSnapshot
                Snapshot() const noexcept;
            LIKESPROGRAM_QUIC_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
