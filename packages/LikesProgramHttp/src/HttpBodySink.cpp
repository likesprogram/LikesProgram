#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Http/HttpBodyBudget.hpp>

#include <LikesProgram/Core/time/Clock.hpp>

#include <algorithm>
#include <deque>
#include <limits>

namespace LikesProgram {
    namespace Http {
        struct HttpBodySink::Impl {
            explicit Impl(HttpBodySinkLimits configuredLimits)
                : limits(configuredLimits), lastActivity(Time::Clock::Now()) {
                if (limits.maxBufferedBytes == 0
                    || limits.lowWatermark > limits.highWatermark
                    || limits.highWatermark > limits.maxBufferedBytes) {
                    state = HttpBodySinkState::Failed;
                    lastError = Status::InvalidArgument(
                        u"HTTP body sink watermarks are out of range");
                }
            }

            ~Impl() {
                ReleaseBuffered();
            }

            HttpBodySinkLimits limits;
            HttpBodySinkState state = HttpBodySinkState::Open;
            Status lastError;
            std::deque<std::vector<std::uint8_t>> chunks;
            std::size_t frontOffset = 0;
            std::size_t bufferedBytes = 0;
            std::uint64_t pulledBytes = 0;
            Time::Deadline deadline = Time::Deadline::Infinite();
            Time::Duration idleTimeout{};
            Time::SteadyTimePoint lastActivity;
            HttpBodyBudget* budget = nullptr;
            std::uint64_t budgetStreamId = 0;
            HttpBodyCancellation* cancellation = nullptr;
            HttpBodyBackpressureAdapter* backpressure = nullptr;
            bool backpressurePaused = false;
            HttpBodyDrainObserver* drainObserver = nullptr;
            std::uint64_t drainObserverStreamId = 0;

            void ReleaseBuffered() noexcept {
                if (budget != nullptr && bufferedBytes != 0) {
                    (void)budget->Release(budgetStreamId, bufferedBytes);
                }
                bufferedBytes = 0;
            }

            bool IsTimedOut() const noexcept {
                if (deadline.Expired()) return true;
                return idleTimeout > Time::Duration::zero()
                    && Time::Clock::Now() - lastActivity >= idleTimeout;
            }

            bool NeedsBackpressure() const noexcept {
                return bufferedBytes >= limits.highWatermark
                    || (budget != nullptr
                        && budget->NeedsPause(budgetStreamId));
            }

            bool CanResumeBackpressure() const noexcept {
                return bufferedBytes <= limits.lowWatermark
                    && (budget == nullptr || budget->CanResume(budgetStreamId));
            }

            void NotifyBackpressure() noexcept {
                if (backpressure == nullptr) return;
                if (NeedsBackpressure() && !backpressurePaused) {
                    backpressurePaused = true;
                    backpressure->Pause();
                    return;
                }
                if (backpressurePaused && CanResumeBackpressure()) {
                    backpressurePaused = false;
                    backpressure->Resume();
                }
            }

            void NotifyDrain(std::size_t bytes) noexcept {
                if (drainObserver == nullptr || bytes == 0) return;
                drainObserver->Observe({ drainObserverStreamId, bytes,
                    pulledBytes, bufferedBytes });
            }

            Result<void> CheckTimeout() {
                if (state == HttpBodySinkState::Failed
                    || state == HttpBodySinkState::Cancelled
                    || state == HttpBodySinkState::Expired
                    || state == HttpBodySinkState::Closed) {
                    return {};
                }
                if (!IsTimedOut()) return {};
                state = HttpBodySinkState::Expired;
                lastError = Status::DeadlineExceeded(
                    u"HTTP body sink deadline or idle timeout expired");
                ReleaseBuffered();
                chunks.clear();
                frontOffset = 0;
                if (cancellation != nullptr) {
                    cancellation->Cancel(HttpBodyCancelReason::DeadlineExceeded);
                }
                return lastError;
            }

            Result<void> CheckWritable() {
                auto timeout = CheckTimeout();
                if (!timeout.IsOk()) return timeout;
                if (state == HttpBodySinkState::Failed) return lastError;
                if (state == HttpBodySinkState::Cancelled) {
                    return Status(StatusCode::Cancelled, u"HTTP body sink is cancelled");
                }
                if (state == HttpBodySinkState::Expired) return lastError;
                if (state == HttpBodySinkState::Closed) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP body sink is closed");
                }
                if (state == HttpBodySinkState::Paused) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP body sink is paused");
                }
                return {};
            }

            void Touch() noexcept {
                lastActivity = Time::Clock::Now();
            }
        };

        HttpBodySink::HttpBodySink(HttpBodySinkLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) { }

        HttpBodySink::~HttpBodySink() = default;

        HttpBodySink::HttpBodySink(HttpBodySink&&) noexcept = default;

        HttpBodySink& HttpBodySink::operator=(HttpBodySink&&) noexcept = default;

        Result<std::size_t> HttpBodySink::Push(
            const std::uint8_t* data, std::size_t size) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (size != 0 && data == nullptr) {
                return Status::InvalidArgument(u"HTTP body sink data is null");
            }
            auto writable = m_impl->CheckWritable();
            if (!writable.IsOk()) return writable.GetStatus();
            if (size == 0) return std::size_t{ 0 };

            auto available = m_impl->limits.maxBufferedBytes - m_impl->bufferedBytes;
            if (m_impl->budget != nullptr) {
                available = std::min(available,
                    m_impl->budget->AvailableBytes(m_impl->budgetStreamId));
            }
            const auto accepted = std::min(size, available);
            if (accepted == 0) {
                m_impl->NotifyBackpressure();
                return std::size_t{ 0 };
            }
            if (m_impl->budget != nullptr) {
                auto reserved = m_impl->budget->Reserve(
                    m_impl->budgetStreamId, accepted);
                if (!reserved.IsOk()) return reserved.GetStatus();
                if (reserved.Value() != accepted) {
                    (void)m_impl->budget->Release(
                        m_impl->budgetStreamId, reserved.Value());
                    return std::size_t{ 0 };
                }
            }
            m_impl->chunks.emplace_back(data, data + accepted);
            m_impl->bufferedBytes += accepted;
            m_impl->Touch();
            m_impl->NotifyBackpressure();
            return accepted;
        }

        Result<std::vector<std::uint8_t>> HttpBodySink::Pull(std::size_t maxBytes) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (maxBytes == 0) return Status::InvalidArgument(
                u"HTTP body sink pull size must be positive");
            auto timeout = m_impl->CheckTimeout();
            if (!timeout.IsOk()) return timeout.GetStatus();
            if (m_impl->state == HttpBodySinkState::Failed) return m_impl->lastError;
            if (m_impl->state == HttpBodySinkState::Cancelled) {
                return Status(StatusCode::Cancelled, u"HTTP body sink is cancelled");
            }
            if (m_impl->state == HttpBodySinkState::Expired) return m_impl->lastError;

            const auto requested = std::min(maxBytes, m_impl->bufferedBytes);
            std::vector<std::uint8_t> result;
            result.reserve(requested);
            std::size_t remaining = requested;
            while (remaining != 0 && !m_impl->chunks.empty()) {
                auto& chunk = m_impl->chunks.front();
                const auto available = chunk.size() - m_impl->frontOffset;
                const auto take = std::min(available, remaining);
                result.insert(result.end(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(m_impl->frontOffset),
                    chunk.begin() + static_cast<std::ptrdiff_t>(m_impl->frontOffset + take));
                m_impl->frontOffset += take;
                m_impl->bufferedBytes -= take;
                remaining -= take;
                if (m_impl->frontOffset == chunk.size()) {
                    m_impl->chunks.pop_front();
                    m_impl->frontOffset = 0;
                }
            }
            if (m_impl->budget != nullptr && !result.empty()) {
                (void)m_impl->budget->Release(
                    m_impl->budgetStreamId, result.size());
            }
            if (!result.empty()) {
                const auto pulled = static_cast<std::uint64_t>(result.size());
                const auto counterCapacity =
                    std::numeric_limits<std::uint64_t>::max()
                    - m_impl->pulledBytes;
                m_impl->pulledBytes += std::min(pulled, counterCapacity);
            }
            if (!result.empty()) m_impl->Touch();
            m_impl->NotifyBackpressure();
            m_impl->NotifyDrain(result.size());
            return result;
        }

        Result<std::vector<std::uint8_t>> HttpBodySink::PeekForProducer(
            std::size_t maxBytes) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (maxBytes == 0) return Status::InvalidArgument(
                u"HTTP body sink peek size must be positive");
            auto timeout = m_impl->CheckTimeout();
            if (!timeout.IsOk()) return timeout.GetStatus();
            if (m_impl->state == HttpBodySinkState::Failed) return m_impl->lastError;
            if (m_impl->state == HttpBodySinkState::Cancelled) {
                return Status(StatusCode::Cancelled, u"HTTP body sink is cancelled");
            }
            if (m_impl->state == HttpBodySinkState::Expired) return m_impl->lastError;

            const auto requested = std::min(maxBytes, m_impl->bufferedBytes);
            std::vector<std::uint8_t> result;
            result.reserve(requested);
            auto chunkIt = m_impl->chunks.begin();
            auto offset = m_impl->frontOffset;
            auto remaining = requested;
            while (remaining != 0 && chunkIt != m_impl->chunks.end()) {
                const auto available = chunkIt->size() - offset;
                const auto take = std::min(available, remaining);
                result.insert(result.end(),
                    chunkIt->begin() + static_cast<std::ptrdiff_t>(offset),
                    chunkIt->begin() + static_cast<std::ptrdiff_t>(offset + take));
                remaining -= take;
                ++chunkIt;
                offset = 0;
            }
            return result;
        }

        Result<void> HttpBodySink::ConsumeForProducer(std::size_t bytes) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (bytes == 0) return Status::InvalidArgument(
                u"HTTP body sink consume size must be positive");
            auto timeout = m_impl->CheckTimeout();
            if (!timeout.IsOk()) return timeout;
            if (m_impl->state == HttpBodySinkState::Failed) return m_impl->lastError;
            if (m_impl->state == HttpBodySinkState::Cancelled) {
                return Status(StatusCode::Cancelled, u"HTTP body sink is cancelled");
            }
            if (m_impl->state == HttpBodySinkState::Expired) return m_impl->lastError;
            if (bytes > m_impl->bufferedBytes) {
                return Status::InvalidArgument(
                    u"HTTP body sink consume exceeds buffered bytes");
            }

            auto remaining = bytes;
            while (remaining != 0 && !m_impl->chunks.empty()) {
                auto& chunk = m_impl->chunks.front();
                const auto available = chunk.size() - m_impl->frontOffset;
                const auto take = std::min(available, remaining);
                m_impl->frontOffset += take;
                m_impl->bufferedBytes -= take;
                remaining -= take;
                if (m_impl->frontOffset == chunk.size()) {
                    m_impl->chunks.pop_front();
                    m_impl->frontOffset = 0;
                }
            }
            if (m_impl->budget != nullptr) {
                (void)m_impl->budget->Release(m_impl->budgetStreamId, bytes);
            }
            m_impl->Touch();
            m_impl->NotifyBackpressure();
            return {};
        }

        Result<bool> HttpBodySink::CanTransferBudgetToAdapter(
            HttpBodyBudget* budget, std::uint64_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (m_impl->budget == nullptr) return false;
            if (budget == nullptr
                || m_impl->budget != budget
                || m_impl->budgetStreamId != streamId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer budget does not match the HTTP/3 adapter");
            }
            return true;
        }

        void HttpBodySink::ConsumeForAdapter(
            std::size_t bytes, bool releaseBudget) noexcept {
            if (!m_impl || bytes == 0 || bytes > m_impl->bufferedBytes) return;
            auto remaining = bytes;
            while (remaining != 0 && !m_impl->chunks.empty()) {
                auto& chunk = m_impl->chunks.front();
                const auto available = chunk.size() - m_impl->frontOffset;
                const auto take = std::min(available, remaining);
                m_impl->frontOffset += take;
                m_impl->bufferedBytes -= take;
                remaining -= take;
                if (m_impl->frontOffset == chunk.size()) {
                    m_impl->chunks.pop_front();
                    m_impl->frontOffset = 0;
                }
            }
            if (releaseBudget && m_impl->budget != nullptr) {
                (void)m_impl->budget->Release(m_impl->budgetStreamId, bytes);
            }
            m_impl->Touch();
            m_impl->NotifyBackpressure();
        }

        Result<bool> HttpBodySink::ValidateRejectedDataRequeueForAdapter(
            std::size_t bytes,
            HttpBodyBudget* budget,
            std::uint64_t streamId,
            bool bodyBudgetBound) const {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (bytes == 0) {
                return Status::InvalidArgument(
                    u"HTTP body producer rejected DATA is empty");
            }
            if (m_impl->state != HttpBodySinkState::Open
                && m_impl->state != HttpBodySinkState::Paused
                && m_impl->state != HttpBodySinkState::Closed) {
                return m_impl->lastError.IsOk()
                    ? Status(StatusCode::FailedPrecondition,
                        u"HTTP body producer cannot requeue rejected DATA")
                    : m_impl->lastError;
            }
            if (m_impl->state != HttpBodySinkState::Closed
                && m_impl->IsTimedOut()) {
                return Status::DeadlineExceeded(
                    u"HTTP body producer deadline or idle timeout expired");
            }
            if (bytes > m_impl->limits.maxBufferedBytes - m_impl->bufferedBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP body producer cannot fit rejected DATA");
            }
            if (m_impl->budget == nullptr) return false;
            if (!bodyBudgetBound
                || budget == nullptr
                || m_impl->budget != budget
                || m_impl->budgetStreamId != streamId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer budget does not match rejected DATA");
            }
            return true;
        }

        Result<void> HttpBodySink::PrependRejectedDataForAdapter(
            const std::vector<std::uint8_t>& payload) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (payload.empty()
                || payload.size()
                    > m_impl->limits.maxBufferedBytes - m_impl->bufferedBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP body producer cannot fit rejected DATA");
            }

            try {
                std::vector<std::uint8_t> remainingFront;
                const auto hadPartialFront = m_impl->frontOffset != 0;
                if (hadPartialFront) {
                    const auto& front = m_impl->chunks.front();
                    remainingFront.assign(
                        front.begin()
                            + static_cast<std::ptrdiff_t>(m_impl->frontOffset),
                        front.end());
                }
                m_impl->chunks.emplace_front(payload);
                if (hadPartialFront) {
                    m_impl->chunks[1].swap(remainingFront);
                    m_impl->frontOffset = 0;
                }
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP body producer rejected DATA allocation failed");
            }
            m_impl->bufferedBytes += payload.size();
            return {};
        }

        void HttpBodySink::FinishRejectedDataRequeueForAdapter() noexcept {
            if (!m_impl) return;
            m_impl->Touch();
            m_impl->NotifyBackpressure();
        }

        Result<void> HttpBodySink::Close() {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            auto timeout = m_impl->CheckTimeout();
            if (!timeout.IsOk()) return timeout;
            if (m_impl->state == HttpBodySinkState::Failed
                || m_impl->state == HttpBodySinkState::Cancelled
                || m_impl->state == HttpBodySinkState::Expired) {
                return m_impl->lastError.IsOk()
                    ? Status(StatusCode::FailedPrecondition,
                        u"HTTP body sink cannot close")
                    : m_impl->lastError;
            }
            m_impl->state = HttpBodySinkState::Closed;
            m_impl->Touch();
            return {};
        }

        void HttpBodySink::Pause() noexcept {
            if (m_impl && m_impl->state == HttpBodySinkState::Open) {
                m_impl->state = HttpBodySinkState::Paused;
            }
        }

        void HttpBodySink::Resume() noexcept {
            if (m_impl && m_impl->state == HttpBodySinkState::Paused) {
                m_impl->state = HttpBodySinkState::Open;
                m_impl->Touch();
            }
        }

        void HttpBodySink::Cancel() noexcept {
            Cancel(HttpBodyCancelReason::Application);
        }

        void HttpBodySink::Cancel(HttpBodyCancelReason reason) noexcept {
            if (!m_impl) return;
            if (m_impl->state == HttpBodySinkState::Closed
                || m_impl->state == HttpBodySinkState::Failed) return;
            m_impl->state = HttpBodySinkState::Cancelled;
            m_impl->lastError = Status(StatusCode::Cancelled,
                u"HTTP body sink was cancelled");
            m_impl->ReleaseBuffered();
            m_impl->chunks.clear();
            m_impl->frontOffset = 0;
            if (m_impl->cancellation != nullptr) {
                m_impl->cancellation->Cancel(reason);
            }
        }

        void HttpBodySink::Reset() noexcept {
            if (!m_impl) return;
            m_impl->ReleaseBuffered();
            m_impl->chunks.clear();
            m_impl->frontOffset = 0;
            m_impl->deadline = Time::Deadline::Infinite();
            m_impl->idleTimeout = Time::Duration{};
            m_impl->lastError = Status();
            m_impl->state = HttpBodySinkState::Open;
            m_impl->Touch();
            const auto wasBackpressurePaused = m_impl->backpressurePaused;
            m_impl->backpressurePaused = false;
            if (wasBackpressurePaused && m_impl->backpressure != nullptr) {
                m_impl->backpressure->Resume();
            }
            if (m_impl->cancellation != nullptr) {
                m_impl->cancellation->Reset();
            }
            if (m_impl->limits.maxBufferedBytes == 0
                || m_impl->limits.lowWatermark > m_impl->limits.highWatermark
                || m_impl->limits.highWatermark > m_impl->limits.maxBufferedBytes) {
                m_impl->state = HttpBodySinkState::Failed;
                m_impl->lastError = Status::InvalidArgument(
                    u"HTTP body sink watermarks are out of range");
            }
        }

        Result<void> HttpBodySink::SetDeadline(Time::Deadline deadline) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            m_impl->deadline = deadline;
            return m_impl->CheckTimeout();
        }

        Result<void> HttpBodySink::SetIdleTimeout(Time::Duration timeout) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (timeout < Time::Duration::zero()) {
                return Status::InvalidArgument(u"HTTP body sink idle timeout is negative");
            }
            m_impl->idleTimeout = timeout;
            m_impl->Touch();
            return m_impl->CheckTimeout();
        }

        Result<void> HttpBodySink::CheckTimeout() {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            return m_impl->CheckTimeout();
        }

        void HttpBodySink::Touch() noexcept {
            if (m_impl && (m_impl->state == HttpBodySinkState::Open
                || m_impl->state == HttpBodySinkState::Paused)) {
                m_impl->Touch();
            }
        }

        Result<void> HttpBodySink::AttachBudget(
            HttpBodyBudget* budget, std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (budget == nullptr) {
                return Status::InvalidArgument(u"HTTP body sink budget is null");
            }
            if (m_impl->budget != nullptr) {
                if (m_impl->budget == budget && m_impl->budgetStreamId == streamId) {
                    return {};
                }
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink already has a budget");
            }
            if (m_impl->bufferedBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink budget must attach before buffering");
            }
            if (m_impl->state == HttpBodySinkState::Failed) return m_impl->lastError;
            if (m_impl->state != HttpBodySinkState::Open
                && m_impl->state != HttpBodySinkState::Paused) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink budget requires an open sink");
            }
            const auto budgetError = budget->LastError();
            if (!budgetError.IsOk()) return budgetError;
            m_impl->budget = budget;
            m_impl->budgetStreamId = streamId;
            return {};
        }

        bool HttpBodySink::HasBudget() const noexcept {
            return m_impl != nullptr && m_impl->budget != nullptr;
        }

        std::uint64_t HttpBodySink::BudgetStreamId() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->budgetStreamId;
        }

        Result<void> HttpBodySink::AttachCancellation(
            HttpBodyCancellation* cancellation) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (cancellation == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP body sink cancellation is null");
            }
            if (m_impl->cancellation != nullptr) {
                if (m_impl->cancellation == cancellation) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink already has cancellation");
            }
            if (m_impl->bufferedBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink cancellation must attach before buffering");
            }
            if (m_impl->state != HttpBodySinkState::Open
                && m_impl->state != HttpBodySinkState::Paused) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink cancellation requires an open sink");
            }
            if (cancellation->IsCancelled()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink cancellation is already cancelled");
            }
            m_impl->cancellation = cancellation;
            return {};
        }

        bool HttpBodySink::HasCancellation() const noexcept {
            return m_impl != nullptr && m_impl->cancellation != nullptr;
        }

        Result<void> HttpBodySink::AttachBackpressure(
            HttpBodyBackpressureAdapter* adapter) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (adapter == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP body sink backpressure adapter is null");
            }
            if (m_impl->backpressure != nullptr) {
                if (m_impl->backpressure == adapter) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink already has backpressure adapter");
            }
            if (m_impl->bufferedBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink backpressure must attach before buffering");
            }
            if (m_impl->state != HttpBodySinkState::Open
                && m_impl->state != HttpBodySinkState::Paused) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink backpressure requires an open sink");
            }
            m_impl->backpressure = adapter;
            m_impl->NotifyBackpressure();
            return {};
        }

        bool HttpBodySink::HasBackpressure() const noexcept {
            return m_impl != nullptr && m_impl->backpressure != nullptr;
        }

        Result<void> HttpBodySink::AttachDrainObserver(
            HttpBodyDrainObserver* observer,
            std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP body sink is moved-from");
            if (observer == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP body sink drain observer is null");
            }
            if (m_impl->drainObserver != nullptr) {
                if (m_impl->drainObserver == observer
                    && m_impl->drainObserverStreamId == streamId) {
                    return {};
                }
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body sink already has a drain observer");
            }
            m_impl->drainObserver = observer;
            m_impl->drainObserverStreamId = streamId;
            return {};
        }

        bool HttpBodySink::HasDrainObserver() const noexcept {
            return m_impl != nullptr && m_impl->drainObserver != nullptr;
        }

        std::uint64_t HttpBodySink::DrainObserverStreamId() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->drainObserverStreamId;
        }

        HttpBodySinkState HttpBodySink::State() const noexcept {
            return m_impl == nullptr ? HttpBodySinkState::Failed : m_impl->state;
        }

        bool HttpBodySink::IsPaused() const noexcept {
            return State() == HttpBodySinkState::Paused;
        }

        bool HttpBodySink::IsCancelled() const noexcept {
            return State() == HttpBodySinkState::Cancelled;
        }

        bool HttpBodySink::IsClosed() const noexcept {
            return State() == HttpBodySinkState::Closed;
        }

        bool HttpBodySink::IsExpired() const noexcept {
            if (!m_impl) return true;
            return m_impl->state == HttpBodySinkState::Expired
                || ((m_impl->state == HttpBodySinkState::Open
                    || m_impl->state == HttpBodySinkState::Paused)
                    && m_impl->IsTimedOut());
        }

        bool HttpBodySink::NeedsPause() const noexcept {
            return m_impl != nullptr
                && (m_impl->bufferedBytes >= m_impl->limits.highWatermark
                    || (m_impl->budget != nullptr
                        && m_impl->budget->NeedsPause(m_impl->budgetStreamId)));
        }

        bool HttpBodySink::CanResume() const noexcept {
            return m_impl != nullptr
                && m_impl->state == HttpBodySinkState::Paused
                && m_impl->CanResumeBackpressure();
        }

        std::size_t HttpBodySink::BufferedBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->bufferedBytes;
        }

        std::size_t HttpBodySink::WritableBytes() const noexcept {
            if (m_impl == nullptr
                || m_impl->state == HttpBodySinkState::Paused
                || m_impl->state == HttpBodySinkState::Closed
                || m_impl->state == HttpBodySinkState::Cancelled
                || m_impl->state == HttpBodySinkState::Expired
                || m_impl->state == HttpBodySinkState::Failed) {
                return 0;
            }
            auto available = m_impl->limits.maxBufferedBytes - m_impl->bufferedBytes;
            if (m_impl->budget != nullptr) {
                available = std::min(available,
                    m_impl->budget->AvailableBytes(m_impl->budgetStreamId));
            }
            return available;
        }

        std::size_t HttpBodySink::MaxBufferedBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->limits.maxBufferedBytes;
        }

        std::uint64_t HttpBodySink::PulledBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->pulledBytes;
        }

        Status HttpBodySink::LastError() const {
            return m_impl == nullptr
                ? Status::Internal(u"HTTP body sink is moved-from")
                : m_impl->lastError;
        }
    }
}
