#include <LikesProgram/Http/HttpBodyBudget.hpp>

#include <algorithm>
#include <unordered_map>

namespace LikesProgram {
    namespace Http {
        namespace {
            HttpBodyBudgetWatermarks DefaultWatermarks(
                HttpBodyBudgetLimits limits) noexcept {
                return HttpBodyBudgetWatermarks{
                    limits.maxConnectionBytes == 0
                        ? 0 : limits.maxConnectionBytes - 1,
                    limits.maxConnectionBytes,
                    limits.maxStreamBytes == 0
                        ? 0 : limits.maxStreamBytes - 1,
                    limits.maxStreamBytes
                };
            }
        }

        struct HttpBodyBudget::Impl {
            struct StreamState {
                std::size_t reservedBytes = 0;
                bool paused = false;
            };

            Impl(HttpBodyBudgetLimits configuredLimits,
                HttpBodyBudgetWatermarks configuredWatermarks)
                : limits(configuredLimits), watermarks(configuredWatermarks) {
                ValidateLimits();
            }

            void ValidateLimits() {
                if (limits.maxConnectionBytes == 0
                    || limits.maxStreamBytes == 0
                    || limits.maxStreamBytes > limits.maxConnectionBytes) {
                    lastError = Status::InvalidArgument(
                        u"HTTP body budget limits are out of range");
                    return;
                }
                if (watermarks.connectionHighBytes == 0
                    || watermarks.connectionLowBytes >= watermarks.connectionHighBytes
                    || watermarks.connectionHighBytes > limits.maxConnectionBytes
                    || watermarks.streamHighBytes == 0
                    || watermarks.streamLowBytes >= watermarks.streamHighBytes
                    || watermarks.streamHighBytes > limits.maxStreamBytes) {
                    lastError = Status::InvalidArgument(
                        u"HTTP body budget watermarks are out of range");
                }
            }

            void NotifyReserved(std::uint64_t streamId) noexcept {
                const auto streamIt = streamBytes.find(streamId);
                if (streamIt == streamBytes.end()) return;
                auto& stream = streamIt->second;
                if (!connectionPaused
                    && reservedBytes >= watermarks.connectionHighBytes) {
                    connectionPaused = true;
                    if (observer != nullptr) observer->PauseConnection();
                }
                if (!stream.paused
                    && stream.reservedBytes >= watermarks.streamHighBytes) {
                    stream.paused = true;
                    if (observer != nullptr) observer->PauseStream(streamId);
                }
            }

            void NotifyReleased(std::uint64_t streamId) noexcept {
                const auto streamIt = streamBytes.find(streamId);
                if (streamIt == streamBytes.end()) return;
                auto& stream = streamIt->second;
                if (stream.paused
                    && stream.reservedBytes <= watermarks.streamLowBytes) {
                    stream.paused = false;
                    if (observer != nullptr) observer->ResumeStream(streamId);
                }
                if (connectionPaused
                    && reservedBytes <= watermarks.connectionLowBytes) {
                    connectionPaused = false;
                    if (observer != nullptr) observer->ResumeConnection();
                }
            }

            HttpBodyBudgetLimits limits;
            HttpBodyBudgetWatermarks watermarks;
            std::unordered_map<std::uint64_t, StreamState> streamBytes;
            std::size_t reservedBytes = 0;
            HttpBodyBudgetObserver* observer = nullptr;
            bool connectionPaused = false;
            Status lastError;
        };

        HttpBodyBudget::HttpBodyBudget(HttpBodyBudgetLimits limits)
            : HttpBodyBudget(limits, DefaultWatermarks(limits)) { }

        HttpBodyBudget::HttpBodyBudget(
            HttpBodyBudgetLimits limits,
            HttpBodyBudgetWatermarks watermarks)
            : m_impl(std::make_unique<Impl>(limits, watermarks)) { }

        HttpBodyBudget::~HttpBodyBudget() = default;

        HttpBodyBudget::HttpBodyBudget(HttpBodyBudget&&) noexcept = default;

        HttpBodyBudget& HttpBodyBudget::operator=(HttpBodyBudget&&) noexcept = default;

        Result<std::size_t> HttpBodyBudget::Reserve(
            std::uint64_t streamId, std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"HTTP body budget is moved-from");
            if (!m_impl->lastError.IsOk()) return m_impl->lastError;
            if (bytes == 0) return std::size_t{ 0 };

            const auto streamIt = m_impl->streamBytes.find(streamId);
            const auto streamUsed = streamIt == m_impl->streamBytes.end()
                ? std::size_t{ 0 } : streamIt->second.reservedBytes;
            const auto connectionAvailable =
                m_impl->limits.maxConnectionBytes - m_impl->reservedBytes;
            const auto streamAvailable = m_impl->limits.maxStreamBytes - streamUsed;
            const auto accepted = std::min(bytes,
                std::min(connectionAvailable, streamAvailable));
            if (accepted == 0) return std::size_t{ 0 };

            auto& stream = m_impl->streamBytes[streamId];
            stream.reservedBytes = streamUsed + accepted;
            m_impl->reservedBytes += accepted;
            m_impl->NotifyReserved(streamId);
            return accepted;
        }

        Result<void> HttpBodyBudget::Release(
            std::uint64_t streamId, std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"HTTP body budget is moved-from");
            if (!m_impl->lastError.IsOk()) return m_impl->lastError;
            if (bytes == 0) return {};

            const auto streamIt = m_impl->streamBytes.find(streamId);
            if (streamIt == m_impl->streamBytes.end()
                || streamIt->second.reservedBytes < bytes) {
                return Status::InvalidArgument(
                    u"HTTP body budget release exceeds stream reservation");
            }
            streamIt->second.reservedBytes -= bytes;
            m_impl->reservedBytes -= bytes;
            m_impl->NotifyReleased(streamId);
            if (streamIt->second.reservedBytes == 0) {
                m_impl->streamBytes.erase(streamIt);
            }
            return {};
        }

        Result<void> HttpBodyBudget::Reset() {
            if (!m_impl) return Status::Internal(u"HTTP body budget is moved-from");
            if (m_impl->reservedBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body budget cannot reset with active reservations");
            }
            m_impl->streamBytes.clear();
            m_impl->connectionPaused = false;
            m_impl->lastError = Status();
            m_impl->ValidateLimits();
            return m_impl->lastError.IsOk() ? Result<void>{} : m_impl->lastError;
        }

        Result<void> HttpBodyBudget::AttachObserver(
            HttpBodyBudgetObserver* observer) {
            if (!m_impl) return Status::Internal(u"HTTP body budget is moved-from");
            if (observer == nullptr) {
                return Status::InvalidArgument(u"HTTP body budget observer is null");
            }
            if (!m_impl->lastError.IsOk()) return m_impl->lastError;
            if (m_impl->observer != nullptr) {
                if (m_impl->observer == observer) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body budget already has an observer");
            }
            if (m_impl->reservedBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body budget observer must attach before reservations");
            }
            m_impl->observer = observer;
            return {};
        }

        bool HttpBodyBudget::HasObserver() const noexcept {
            return m_impl != nullptr && m_impl->observer != nullptr;
        }

        std::size_t HttpBodyBudget::ReservedBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->reservedBytes;
        }

        std::size_t HttpBodyBudget::ReservedBytes(std::uint64_t streamId) const noexcept {
            if (!m_impl) return 0;
            const auto it = m_impl->streamBytes.find(streamId);
            return it == m_impl->streamBytes.end()
                ? 0 : it->second.reservedBytes;
        }

        std::size_t HttpBodyBudget::AvailableBytes(std::uint64_t streamId) const noexcept {
            if (!m_impl || !m_impl->lastError.IsOk()) return 0;
            const auto streamUsed = ReservedBytes(streamId);
            const auto connectionAvailable =
                m_impl->limits.maxConnectionBytes - m_impl->reservedBytes;
            const auto streamAvailable = m_impl->limits.maxStreamBytes - streamUsed;
            return std::min(connectionAvailable, streamAvailable);
        }

        std::size_t HttpBodyBudget::MaxConnectionBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->limits.maxConnectionBytes;
        }

        std::size_t HttpBodyBudget::MaxStreamBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->limits.maxStreamBytes;
        }

        HttpBodyBudgetWatermarks HttpBodyBudget::Watermarks() const noexcept {
            return m_impl == nullptr ? HttpBodyBudgetWatermarks{} : m_impl->watermarks;
        }

        bool HttpBodyBudget::NeedsPause(std::uint64_t streamId) const noexcept {
            if (!m_impl || !m_impl->lastError.IsOk()) return false;
            return m_impl->reservedBytes >= m_impl->watermarks.connectionHighBytes
                || ReservedBytes(streamId) >= m_impl->watermarks.streamHighBytes;
        }

        bool HttpBodyBudget::CanResume(std::uint64_t streamId) const noexcept {
            if (!m_impl || !m_impl->lastError.IsOk()) return false;
            return m_impl->reservedBytes <= m_impl->watermarks.connectionLowBytes
                && ReservedBytes(streamId) <= m_impl->watermarks.streamLowBytes;
        }

        HttpBodyBudgetSnapshot HttpBodyBudget::Snapshot() const noexcept {
            if (!m_impl) return {};
            return HttpBodyBudgetSnapshot{
                m_impl->limits.maxConnectionBytes,
                m_impl->limits.maxStreamBytes,
                m_impl->reservedBytes,
                m_impl->streamBytes.size()
            };
        }

        Status HttpBodyBudget::LastError() const {
            return m_impl == nullptr
                ? Status::Internal(u"HTTP body budget is moved-from")
                : m_impl->lastError;
        }
    }
}
