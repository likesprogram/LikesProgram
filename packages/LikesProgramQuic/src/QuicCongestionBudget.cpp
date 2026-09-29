#include <LikesProgram/Quic/QuicCongestionBudget.hpp>

#include <limits>
#include <unordered_map>

namespace LikesProgram {
    namespace Quic {
        struct QuicCongestionBudget::Impl {
            QuicCongestionBudgetLimits limits;
            std::size_t windowBytes = 0;
            std::size_t inFlightBytes = 0;
            std::unordered_map<std::uint64_t, std::size_t> reservations;
        };

        QuicCongestionBudget::QuicCongestionBudget(
            QuicCongestionBudgetLimits limits) noexcept
            : m_impl(std::make_unique<Impl>(Impl{ limits, limits.maxWindowBytes, 0, {} })) {}

        QuicCongestionBudget::~QuicCongestionBudget() = default;
        QuicCongestionBudget::QuicCongestionBudget(
            QuicCongestionBudget&&) noexcept = default;
        QuicCongestionBudget& QuicCongestionBudget::operator=(
            QuicCongestionBudget&&) noexcept = default;

        Result<void> QuicCongestionBudget::SetWindow(std::size_t windowBytes) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC congestion budget is moved-from");
            if (windowBytes < m_impl->inFlightBytes
                || windowBytes > m_impl->limits.maxWindowBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC congestion window cannot cover current in-flight bytes");
            }
            m_impl->windowBytes = windowBytes;
            return {};
        }

        Result<void> QuicCongestionBudget::Reserve(
            std::uint64_t packetNumber,
            std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"QUIC congestion budget is moved-from");
            if (bytes == 0) {
                return Status::InvalidArgument(
                    u"QUIC congestion reservation must have bytes");
            }
            if (m_impl->reservations.find(packetNumber) != m_impl->reservations.end()) {
                return Status(StatusCode::AlreadyExists,
                    u"QUIC congestion reservation is duplicated");
            }
            if (m_impl->reservations.size() >= m_impl->limits.maxReservations
                || bytes > m_impl->windowBytes - m_impl->inFlightBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC congestion window has no available reservation capacity");
            }
            try {
                m_impl->reservations.emplace(packetNumber, bytes);
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC congestion reservation allocation failed");
            }
            m_impl->inFlightBytes += bytes;
            return {};
        }

        Result<void> QuicCongestionBudget::Acknowledge(
            std::uint64_t packetNumber) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC congestion budget is moved-from");
            const auto entry = m_impl->reservations.find(packetNumber);
            if (entry == m_impl->reservations.end()) {
                return Status(StatusCode::NotFound,
                    u"QUIC congestion reservation is not tracked");
            }
            m_impl->inFlightBytes -= entry->second;
            m_impl->reservations.erase(entry);
            return {};
        }

        Result<void> QuicCongestionBudget::Lose(
            std::uint64_t packetNumber) noexcept {
            return Acknowledge(packetNumber);
        }

        QuicCongestionBudgetLimits QuicCongestionBudget::Limits() const noexcept {
            return m_impl ? m_impl->limits : QuicCongestionBudgetLimits{};
        }

        QuicCongestionBudgetSnapshot QuicCongestionBudget::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->windowBytes, m_impl->inFlightBytes,
                m_impl->windowBytes - m_impl->inFlightBytes,
                m_impl->reservations.size() };
        }

        void QuicCongestionBudget::Reset() noexcept {
            if (!m_impl) return;
            m_impl->windowBytes = m_impl->limits.maxWindowBytes;
            m_impl->inFlightBytes = 0;
            m_impl->reservations.clear();
        }
    }
}
