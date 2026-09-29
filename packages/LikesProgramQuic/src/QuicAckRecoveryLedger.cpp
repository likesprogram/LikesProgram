#include <LikesProgram/Quic/QuicAckRecoveryLedger.hpp>

#include <utility>

namespace LikesProgram {
    namespace Quic {
        struct QuicAckRecoveryLedger::Impl {
            explicit Impl(QuicAckRecoveryLimits value) noexcept
                : limits(value), retransmission(value.retransmission) {}

            QuicAckRecoveryLimits limits;
            QuicAckObservationState acknowledgement;
            QuicRetransmissionQueue retransmission;
        };

        QuicAckRecoveryLedger::QuicAckRecoveryLedger(
            QuicAckRecoveryLimits limits) noexcept
            : m_impl(std::make_unique<Impl>(limits)) {}

        QuicAckRecoveryLedger::~QuicAckRecoveryLedger() = default;

        QuicAckRecoveryLedger::QuicAckRecoveryLedger(
            QuicAckRecoveryLedger&&) noexcept = default;

        QuicAckRecoveryLedger& QuicAckRecoveryLedger::operator=(
            QuicAckRecoveryLedger&&) noexcept = default;

        Result<void> QuicAckRecoveryLedger::TrackSent(
            std::uint64_t packetNumber,
            const std::vector<std::uint8_t>& payload,
            Time::SteadyTimePoint expiry) {
            if (!m_impl) return Status::Internal(u"QUIC ACK ledger is moved-from");
            return m_impl->retransmission.Track(packetNumber, payload, expiry);
        }

        Result<QuicAckRecoveryResult> QuicAckRecoveryLedger::ApplyAck(
            const QuicAckFrame& frame) {
            if (!m_impl) return Status::Internal(u"QUIC ACK ledger is moved-from");
            return ApplyAckInternal(frame, nullptr);
        }

        Result<QuicAckRecoveryResult> QuicAckRecoveryLedger::ApplyAck(
            const QuicAckFrame& frame,
            const QuicAckDelayContext& delayContext) {
            if (!m_impl) return Status::Internal(u"QUIC ACK ledger is moved-from");
            return ApplyAckInternal(frame, &delayContext);
        }

        Result<QuicAckRecoveryResult> QuicAckRecoveryLedger::ApplyAckInternal(
            const QuicAckFrame& frame,
            const QuicAckDelayContext* delayContext) {
            auto candidate = m_impl->acknowledgement;
            const auto observation = delayContext
                ? ObserveQuicAck(candidate, frame, *delayContext)
                : ObserveQuicAck(candidate, frame);
            if (!observation.Succeeded()) {
                return Status::InvalidArgument(
                    u"QUIC ACK observation was rejected before queue mutation");
            }

            const auto application = ApplyQuicAckFrame(
                m_impl->retransmission, frame, m_impl->limits.acknowledgement);
            if (!application.IsOk()) return application.GetStatus();

            m_impl->acknowledgement = candidate;
            return QuicAckRecoveryResult{
                observation,
                application.Value(),
                QuicAckRecoverySnapshot{
                    m_impl->acknowledgement,
                    m_impl->retransmission.Snapshot()
                }
            };
        }

        Result<std::vector<QuicRetransmissionPacket>>
            QuicAckRecoveryLedger::CollectExpired(Time::SteadyTimePoint now) {
            if (!m_impl) return Status::Internal(u"QUIC ACK ledger is moved-from");
            return m_impl->retransmission.CollectExpired(now);
        }

        QuicAckRecoveryLimits QuicAckRecoveryLedger::Limits() const noexcept {
            return m_impl ? m_impl->limits : QuicAckRecoveryLimits{};
        }

        QuicAckRecoverySnapshot QuicAckRecoveryLedger::Snapshot() const noexcept {
            return m_impl ? QuicAckRecoverySnapshot{
                m_impl->acknowledgement,
                m_impl->retransmission.Snapshot()
            } : QuicAckRecoverySnapshot{};
        }

        void QuicAckRecoveryLedger::Reset() noexcept {
            if (!m_impl) return;
            m_impl->acknowledgement.Reset();
            m_impl->retransmission.Reset();
        }
    }
}
