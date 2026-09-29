#include <LikesProgram/Quic/QuicCreditCongestionLedger.hpp>

#include <map>
#include <utility>

namespace LikesProgram {
    namespace Quic {
        struct QuicCreditCongestionLedger::Impl {
            struct Reservation {
                std::uint64_t streamId = 0;
                std::size_t bytes = 0;
            };

            explicit Impl(QuicCreditCongestionLedgerLimits value) noexcept
                : limits(value), congestion(value.congestion) {}

            QuicCreditCongestionLedgerLimits limits;
            QuicCongestionBudget congestion;
            QuicConnectionCreditState connection;
            std::map<std::uint64_t, QuicStreamCreditState> streams;
            std::map<std::uint64_t, Reservation> reservations;
        };

        namespace {
            Result<void> ConnectionStatus(QuicConnectionCreditResult result) {
                if (result.Succeeded()) return {};
                if (result.error == QuicConnectionCreditError::InsufficientCredit) {
                    return Status(StatusCode::ResourceExhausted,
                        u"QUIC connection credit is insufficient");
                }
                return Status::InvalidArgument(u"QUIC connection credit update is invalid");
            }

            Result<void> StreamStatus(QuicStreamCreditResult result) {
                if (result.Succeeded()) return {};
                if (result.error == QuicStreamCreditError::InsufficientCredit) {
                    return Status(StatusCode::ResourceExhausted,
                        u"QUIC stream credit is insufficient");
                }
                return Status::InvalidArgument(u"QUIC stream credit update is invalid");
            }

        }

        QuicCreditCongestionLedger::QuicCreditCongestionLedger(
            QuicCreditCongestionLedgerLimits limits) noexcept
            : m_impl(std::make_unique<Impl>(limits)) {}

        QuicCreditCongestionLedger::~QuicCreditCongestionLedger() = default;

        QuicCreditCongestionLedger::QuicCreditCongestionLedger(
            QuicCreditCongestionLedger&&) noexcept = default;

        QuicCreditCongestionLedger& QuicCreditCongestionLedger::operator=(
            QuicCreditCongestionLedger&&) noexcept = default;

        Result<void> QuicCreditCongestionLedger::SetCongestionWindow(
            std::size_t windowBytes) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            return m_impl->congestion.SetWindow(windowBytes);
        }

        Result<void> QuicCreditCongestionLedger::ApplyPeerFlowControl(
            const QuicFlowControlFrame& frame) {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            if (frame.kind == QuicFlowControlFrameKind::MaxData) {
                return ConnectionStatus(
                    ApplyQuicPeerConnectionCredit(m_impl->connection, frame.limit));
            }
            if (frame.kind != QuicFlowControlFrameKind::MaxStreamData) {
                return Status::InvalidArgument(
                    u"QUIC credit ledger accepts only MAX_DATA or MAX_STREAM_DATA");
            }
            auto stream = m_impl->streams.find(frame.streamId);
            if (stream == m_impl->streams.end()) {
                if (m_impl->streams.size() >= m_impl->limits.maxTrackedStreams) {
                    return Status(StatusCode::ResourceExhausted,
                        u"QUIC stream credit ledger limit is exhausted");
                }
                try {
                    stream = m_impl->streams.emplace(
                        frame.streamId, QuicStreamCreditState{}).first;
                }
                catch (...) {
                    return Status(StatusCode::ResourceExhausted,
                        u"QUIC stream credit allocation failed");
                }
            }
            return StreamStatus(
                ApplyQuicPeerStreamCredit(stream->second, frame.limit));
        }

        Result<void> QuicCreditCongestionLedger::Reserve(
            std::uint64_t packetNumber,
            std::uint64_t streamId,
            std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            if (bytes == 0 || m_impl->reservations.size() >= m_impl->limits.maxReservations) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC credit reservation limit is exhausted");
            }
            if (m_impl->reservations.find(packetNumber) != m_impl->reservations.end()) {
                return Status(StatusCode::AlreadyExists,
                    u"QUIC credit packet number is already reserved");
            }
            if (m_impl->streams.find(streamId) == m_impl->streams.end()) {
                return Status::InvalidArgument(u"QUIC stream credit is not configured");
            }
            const auto streamSnapshot = m_impl->streams.find(streamId);
            if (bytes > m_impl->connection.Available()
                || bytes > streamSnapshot->second.Available()
                || bytes > m_impl->congestion.Snapshot().availableBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC connection, stream, or congestion credit is insufficient");
            }

            auto stream = streamSnapshot;
            const auto connection = ReserveQuicConnectionCredit(
                m_impl->connection, bytes);
            if (!connection.Succeeded()) return ConnectionStatus(connection);
            const auto streamResult = ReserveQuicStreamCredit(stream->second, bytes);
            if (!streamResult.Succeeded()) {
                ReleaseQuicConnectionCredit(m_impl->connection, bytes);
                return StreamStatus(streamResult);
            }
            const auto congestion = m_impl->congestion.Reserve(packetNumber, bytes);
            if (!congestion.IsOk()) {
                ReleaseQuicStreamCredit(stream->second, bytes);
                ReleaseQuicConnectionCredit(m_impl->connection, bytes);
                return congestion.GetStatus();
            }
            try {
                m_impl->reservations.emplace(packetNumber,
                    Impl::Reservation{ streamId, bytes });
            }
            catch (...) {
                m_impl->congestion.Lose(packetNumber);
                ReleaseQuicStreamCredit(stream->second, bytes);
                ReleaseQuicConnectionCredit(m_impl->connection, bytes);
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC credit reservation allocation failed");
            }
            return {};
        }

        Result<void> QuicCreditCongestionLedger::Acknowledge(
            std::uint64_t packetNumber) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            const auto reservation = m_impl->reservations.find(packetNumber);
            if (reservation == m_impl->reservations.end()) {
                return Status(StatusCode::NotFound,
                    u"QUIC credit reservation is not tracked");
            }
            const auto stream = m_impl->streams.find(reservation->second.streamId);
            if (stream == m_impl->streams.end()) {
                return Status::Internal(u"QUIC stream credit reservation is inconsistent");
            }
            const auto budget = m_impl->congestion.Acknowledge(packetNumber);
            if (!budget.IsOk()) return budget.GetStatus();
            if (!ReleaseQuicStreamCredit(stream->second, reservation->second.bytes).Succeeded()
                || !ReleaseQuicConnectionCredit(
                    m_impl->connection, reservation->second.bytes).Succeeded()) {
                return Status::Internal(u"QUIC credit release is inconsistent");
            }
            m_impl->reservations.erase(reservation);
            return {};
        }

        Result<void> QuicCreditCongestionLedger::Lose(
            std::uint64_t packetNumber) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            const auto reservation = m_impl->reservations.find(packetNumber);
            if (reservation == m_impl->reservations.end()) {
                return Status(StatusCode::NotFound,
                    u"QUIC credit reservation is not tracked");
            }
            const auto stream = m_impl->streams.find(reservation->second.streamId);
            if (stream == m_impl->streams.end()) {
                return Status::Internal(u"QUIC stream credit reservation is inconsistent");
            }
            const auto budget = m_impl->congestion.Lose(packetNumber);
            if (!budget.IsOk()) return budget.GetStatus();
            if (!ReleaseQuicStreamCredit(stream->second, reservation->second.bytes).Succeeded()
                || !ReleaseQuicConnectionCredit(
                    m_impl->connection, reservation->second.bytes).Succeeded()) {
                return Status::Internal(u"QUIC credit release is inconsistent");
            }
            m_impl->reservations.erase(reservation);
            return {};
        }

        Result<QuicStreamCreditState> QuicCreditCongestionLedger::StreamCredit(
            std::uint64_t streamId) const noexcept {
            if (!m_impl) return Status::Internal(u"QUIC credit ledger is moved-from");
            const auto stream = m_impl->streams.find(streamId);
            if (stream == m_impl->streams.end()) {
                return Status(StatusCode::NotFound,
                    u"QUIC stream credit is not tracked");
            }
            return stream->second;
        }

        QuicCreditCongestionLedgerLimits
            QuicCreditCongestionLedger::Limits() const noexcept {
            return m_impl ? m_impl->limits : QuicCreditCongestionLedgerLimits{};
        }

        QuicCreditCongestionLedgerSnapshot
            QuicCreditCongestionLedger::Snapshot() const noexcept {
            return m_impl ? QuicCreditCongestionLedgerSnapshot{
                m_impl->congestion.Snapshot(),
                m_impl->connection,
                m_impl->streams.size(),
                m_impl->reservations.size()
            } : QuicCreditCongestionLedgerSnapshot{};
        }

        void QuicCreditCongestionLedger::Reset() noexcept {
            if (!m_impl) return;
            m_impl->congestion.Reset();
            m_impl->connection.Reset();
            m_impl->streams.clear();
            m_impl->reservations.clear();
        }
    }
}
