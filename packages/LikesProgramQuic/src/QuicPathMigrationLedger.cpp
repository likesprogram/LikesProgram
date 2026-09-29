#include <LikesProgram/Quic/QuicPathMigrationLedger.hpp>

#include <utility>

namespace LikesProgram {
    namespace Quic {
        struct QuicPathMigrationLedger::Impl {
            QuicPathProbeState probe;
            QuicConnectionIdObservationState connectionId;
        };

        QuicPathMigrationLedger::QuicPathMigrationLedger() noexcept
            : m_impl(std::make_unique<Impl>()) {}

        QuicPathMigrationLedger::~QuicPathMigrationLedger() = default;

        QuicPathMigrationLedger::QuicPathMigrationLedger(
            QuicPathMigrationLedger&&) noexcept = default;

        QuicPathMigrationLedger& QuicPathMigrationLedger::operator=(
            QuicPathMigrationLedger&&) noexcept = default;

        Result<QuicPathMigrationChallengeResult>
            QuicPathMigrationLedger::ObserveChallenge(
                const QuicPathValidationFrame& frame) {
            if (!m_impl) return Status::Internal(u"QUIC path ledger is moved-from");
            if (frame.kind != QuicPathValidationFrameKind::PathChallenge) {
                return Status::InvalidArgument(
                    u"QUIC path challenge observation requires PATH_CHALLENGE");
            }
            auto candidate = m_impl->probe;
            const auto probe = ObserveQuicPathChallenge(candidate, frame.data);
            const auto action = BuildQuicPathResponseAction(frame);
            if (!action.IsOk()) return action.GetStatus();
            m_impl->probe = candidate;
            return QuicPathMigrationChallengeResult{ probe, action.Value() };
        }

        Result<QuicPathProbeResult> QuicPathMigrationLedger::BeginValidation(
            const std::array<std::uint8_t, 8>& token) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC path ledger is moved-from");
            auto candidate = m_impl->probe;
            const auto result = ObserveQuicPathChallenge(candidate, token);
            m_impl->probe = candidate;
            return result;
        }

        Result<QuicPathProbeResult> QuicPathMigrationLedger::ObserveResponse(
            const QuicPathValidationFrame& frame) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC path ledger is moved-from");
            if (frame.kind != QuicPathValidationFrameKind::PathResponse) {
                return Status::InvalidArgument(
                    u"QUIC path response observation requires PATH_RESPONSE");
            }
            auto candidate = m_impl->probe;
            const auto result = ObserveQuicPathResponse(candidate, frame.data);
            if (!result.Succeeded()) {
                return Status::InvalidArgument(
                    u"QUIC path response token was not accepted");
            }
            m_impl->probe = candidate;
            return result;
        }

        void QuicPathMigrationLedger::CancelValidation() noexcept {
            if (!m_impl) return;
            m_impl->probe.Reset();
        }

        Result<QuicConnectionIdObservationResult>
            QuicPathMigrationLedger::ObserveConnectionId(
                const QuicConnectionIdFrame& frame) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC path ledger is moved-from");
            auto candidate = m_impl->connectionId;
            const auto result = ObserveQuicConnectionIdFrame(candidate, frame);
            if (!result.Succeeded()) {
                return Status::InvalidArgument(
                    u"QUIC connection-ID observation was rejected");
            }
            m_impl->connectionId = candidate;
            return result;
        }

        QuicPathMigrationLedgerSnapshot QuicPathMigrationLedger::Snapshot() const noexcept {
            return m_impl ? QuicPathMigrationLedgerSnapshot{
                m_impl->probe, m_impl->connectionId } : QuicPathMigrationLedgerSnapshot{};
        }

        void QuicPathMigrationLedger::Reset() noexcept {
            if (!m_impl) return;
            m_impl->probe.Reset();
            m_impl->connectionId.Reset();
        }
    }
}
