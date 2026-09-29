#include <LikesProgram/Quic/QuicRetransmissionQueue.hpp>

#include <limits>
#include <utility>

namespace {
    constexpr std::uint64_t kQuicPacketNumberMaximum =
        (std::uint64_t{ 1 } << 62) - 1;
}

namespace LikesProgram {
    namespace Quic {
        struct QuicRetransmissionQueue::Impl {
            QuicRetransmissionLimits limits;
            std::map<std::uint64_t, QuicRetransmissionPacket> packets;
            std::size_t trackedBytes = 0;
        };

        QuicRetransmissionQueue::QuicRetransmissionQueue(
            QuicRetransmissionLimits limits) noexcept
            : m_impl(std::make_unique<Impl>(Impl{ limits, {}, 0 })) {}

        QuicRetransmissionQueue::~QuicRetransmissionQueue() = default;

        QuicRetransmissionQueue::QuicRetransmissionQueue(
            QuicRetransmissionQueue&&) noexcept = default;

        QuicRetransmissionQueue& QuicRetransmissionQueue::operator=(
            QuicRetransmissionQueue&&) noexcept = default;

        Result<void> QuicRetransmissionQueue::Track(
            std::uint64_t packetNumber,
            const std::vector<std::uint8_t>& payload,
            Time::SteadyTimePoint expiry) {
            if (!m_impl) return Status::Internal(u"QUIC retransmission queue is moved-from");
            if (packetNumber > kQuicPacketNumberMaximum || payload.empty()
                || m_impl->limits.maxPackets == 0
                || m_impl->limits.maxPayloadBytes < payload.size()) {
                return Status::InvalidArgument(
                    u"QUIC retransmission packet is outside the configured limits");
            }
            if (m_impl->packets.find(packetNumber) != m_impl->packets.end()) {
                return Status(StatusCode::AlreadyExists,
                    u"QUIC retransmission packet number is duplicated");
            }
            if (m_impl->packets.size() >= m_impl->limits.maxPackets
                || m_impl->trackedBytes > m_impl->limits.maxPayloadBytes - payload.size()) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC retransmission queue limit exceeded");
            }
            try {
                auto inserted = m_impl->packets.emplace(packetNumber,
                    QuicRetransmissionPacket{ packetNumber, payload, expiry });
                if (!inserted.second) {
                    return Status(StatusCode::AlreadyExists,
                        u"QUIC retransmission packet number is duplicated");
                }
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC retransmission packet allocation failed");
            }
            m_impl->trackedBytes += payload.size();
            return {};
        }

        Result<void> QuicRetransmissionQueue::Acknowledge(
            std::uint64_t packetNumber) noexcept {
            if (!m_impl) return Status::Internal(u"QUIC retransmission queue is moved-from");
            const auto entry = m_impl->packets.find(packetNumber);
            if (entry == m_impl->packets.end()) {
                return Status(StatusCode::NotFound,
                    u"QUIC retransmission packet number is not tracked");
            }
            m_impl->trackedBytes -= entry->second.payload.size();
            m_impl->packets.erase(entry);
            return {};
        }

        Result<std::vector<QuicRetransmissionPacket>>
            QuicRetransmissionQueue::CollectExpired(Time::SteadyTimePoint now) {
            if (!m_impl) return Status::Internal(u"QUIC retransmission queue is moved-from");
            std::vector<QuicRetransmissionPacket> expired;
            try {
                for (auto it = m_impl->packets.begin(); it != m_impl->packets.end();) {
                    if (it->second.expiry > now) {
                        ++it;
                        continue;
                    }
                    m_impl->trackedBytes -= it->second.payload.size();
                    expired.push_back(std::move(it->second));
                    it = m_impl->packets.erase(it);
                }
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"QUIC expired packet extraction failed");
            }
            return expired;
        }

        QuicRetransmissionSnapshot QuicRetransmissionQueue::Snapshot() const noexcept {
            return m_impl ? QuicRetransmissionSnapshot{
                m_impl->packets.size(), m_impl->trackedBytes } : QuicRetransmissionSnapshot{};
        }

        QuicRetransmissionLimits QuicRetransmissionQueue::Limits() const noexcept {
            return m_impl ? m_impl->limits : QuicRetransmissionLimits{};
        }

        void QuicRetransmissionQueue::Reset() noexcept {
            if (!m_impl) return;
            m_impl->packets.clear();
            m_impl->trackedBytes = 0;
        }
    }
}
