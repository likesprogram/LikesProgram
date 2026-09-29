#include <LikesProgram/Http/HttpBodyProducer.hpp>

#include <algorithm>

namespace LikesProgram {
    namespace Http {
        struct HttpBodyProducer::Impl {
            explicit Impl(HttpBodyProducerLimits limits)
                : queue(limits) { }

            void ClearPreparedPull() noexcept {
                preparedPullId = 0;
                preparedPullBytes = 0;
            }

            void ClearRejectedPrefix() noexcept {
                rejectedPrefixBytes = 0;
                rejectedPrefixStreamId = 0;
                lowestRejectedActionId = 0;
            }

            void ConsumeRejectedPrefix(std::size_t bytes) noexcept {
                const auto consumed = std::min(bytes, rejectedPrefixBytes);
                rejectedPrefixBytes -= consumed;
                if (rejectedPrefixBytes == 0) ClearRejectedPrefix();
            }

            void SynchronizeRejectedPrefix() noexcept {
                if (rejectedPrefixBytes > queue.BufferedBytes()) {
                    rejectedPrefixBytes = queue.BufferedBytes();
                    if (rejectedPrefixBytes == 0) ClearRejectedPrefix();
                }
            }

            void SynchronizePreparedPull() noexcept {
                if (preparedPullId != 0
                    && queue.BufferedBytes() < preparedPullBytes) {
                    ClearPreparedPull();
                }
                SynchronizeRejectedPrefix();
            }

            HttpBodySink queue;
            std::uint64_t nextPreparedPullId = 1;
            std::uint64_t preparedPullId = 0;
            std::size_t preparedPullBytes = 0;
            std::size_t rejectedPrefixBytes = 0;
            std::uint64_t rejectedPrefixStreamId = 0;
            std::uint64_t lowestRejectedActionId = 0;
        };

        HttpBodyProducer::HttpBodyProducer(HttpBodyProducerLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) { }

        HttpBodyProducer::~HttpBodyProducer() = default;

        HttpBodyProducer::HttpBodyProducer(HttpBodyProducer&&) noexcept = default;

        HttpBodyProducer& HttpBodyProducer::operator=(HttpBodyProducer&&) noexcept = default;

        Result<std::size_t> HttpBodyProducer::Push(
            const std::uint8_t* data, std::size_t size) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            auto result = m_impl->queue.Push(data, size);
            m_impl->SynchronizePreparedPull();
            return result;
        }

        Result<std::vector<std::uint8_t>> HttpBodyProducer::Pull(std::size_t maxBytes) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (m_impl->preparedPullId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer already has a prepared pull");
            }
            auto result = m_impl->queue.Pull(maxBytes);
            if (result.IsOk()) {
                m_impl->ConsumeRejectedPrefix(result.Value().size());
            }
            else {
                m_impl->SynchronizeRejectedPrefix();
            }
            return result;
        }

        Result<HttpBodyProducerPreparedPull> HttpBodyProducer::PreparePull(
            std::size_t maxBytes) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (m_impl->preparedPullId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer already has a prepared pull");
            }
            if (m_impl->nextPreparedPullId == 0) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP body producer prepared-pull id space is exhausted");
            }
            auto payload = m_impl->queue.PeekForProducer(maxBytes);
            if (!payload.IsOk()) {
                m_impl->SynchronizePreparedPull();
                return payload.GetStatus();
            }
            if (payload.Value().empty()) return HttpBodyProducerPreparedPull{};

            const auto id = m_impl->nextPreparedPullId++;
            m_impl->preparedPullId = id;
            m_impl->preparedPullBytes = payload.Value().size();
            HttpBodyProducerPreparedPull prepared;
            prepared.available = true;
            prepared.id = id;
            prepared.payload = std::move(payload.Value());
            return Result<HttpBodyProducerPreparedPull>(std::move(prepared));
        }

        Result<HttpBodyProducerPreparedPull>
            HttpBodyProducer::PrepareRejectedDataReplay() {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (m_impl->preparedPullId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer already has a prepared pull");
            }
            if (m_impl->rejectedPrefixBytes == 0) {
                return HttpBodyProducerPreparedPull{};
            }
            return PreparePull(m_impl->rejectedPrefixBytes);
        }

        Result<void> HttpBodyProducer::CommitPull(std::uint64_t id) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (id == 0) return Status::InvalidArgument(
                u"HTTP body producer prepared-pull id is zero");
            if (m_impl->preparedPullId == 0 || id != m_impl->preparedPullId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer prepared-pull id is not active");
            }
            auto result = m_impl->queue.ConsumeForProducer(
                m_impl->preparedPullBytes);
            if (result.IsOk()) {
                m_impl->ConsumeRejectedPrefix(m_impl->preparedPullBytes);
                m_impl->ClearPreparedPull();
            } else {
                m_impl->SynchronizePreparedPull();
            }
            return result;
        }

        Result<void> HttpBodyProducer::RollbackPull(std::uint64_t id) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (id == 0) return Status::InvalidArgument(
                u"HTTP body producer prepared-pull id is zero");
            if (m_impl->preparedPullId == 0 || id != m_impl->preparedPullId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer prepared-pull id is not active");
            }
            m_impl->ClearPreparedPull();
            return {};
        }

        bool HttpBodyProducer::HasPreparedPull() const noexcept {
            return m_impl != nullptr && m_impl->preparedPullId != 0;
        }

        std::size_t HttpBodyProducer::PreparedPullBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->preparedPullBytes;
        }

        bool HttpBodyProducer::HasRejectedDataReplay() const noexcept {
            return m_impl != nullptr && m_impl->rejectedPrefixBytes != 0;
        }

        std::size_t HttpBodyProducer::RejectedDataReplayBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->rejectedPrefixBytes;
        }

        HttpBodyRejectedDataReplaySnapshot
            HttpBodyProducer::RejectedDataReplay() const noexcept {
            if (m_impl == nullptr || m_impl->rejectedPrefixBytes == 0) return {};
            return { true, m_impl->rejectedPrefixStreamId,
                m_impl->lowestRejectedActionId, m_impl->rejectedPrefixBytes };
        }

        Result<bool> HttpBodyProducer::ValidatePreparedPullForAdapter(
            std::uint64_t id,
            HttpBodyBudget* budget,
            std::uint64_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (id == 0) return Status::InvalidArgument(
                u"HTTP body producer prepared-pull id is zero");
            if (m_impl->preparedPullId == 0 || id != m_impl->preparedPullId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer prepared-pull id is not active");
            }
            return m_impl->queue.CanTransferBudgetToAdapter(budget, streamId);
        }

        Result<std::vector<std::uint8_t>>
            HttpBodyProducer::CopyPreparedPullForAdapter(std::uint64_t id) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (id == 0) return Status::InvalidArgument(
                u"HTTP body producer prepared-pull id is zero");
            if (m_impl->preparedPullId == 0 || id != m_impl->preparedPullId) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer prepared-pull id is not active");
            }
            auto payload = m_impl->queue.PeekForProducer(
                m_impl->preparedPullBytes);
            if (!payload.IsOk()) {
                m_impl->SynchronizePreparedPull();
                return payload.GetStatus();
            }
            if (payload.Value().size() != m_impl->preparedPullBytes) {
                m_impl->SynchronizePreparedPull();
                return Status::Internal(
                    u"HTTP body producer prepared prefix changed unexpectedly");
            }
            return payload;
        }

        void HttpBodyProducer::CommitPreparedPullForAdapter(
            bool transferBudget) noexcept {
            if (!m_impl || m_impl->preparedPullId == 0) return;
            const auto consumed = m_impl->preparedPullBytes;
            m_impl->queue.ConsumeForAdapter(
                consumed, !transferBudget);
            m_impl->ConsumeRejectedPrefix(consumed);
            m_impl->ClearPreparedPull();
        }

        Result<bool> HttpBodyProducer::ValidateRejectedDataRequeueForAdapter(
            std::size_t bytes,
            HttpBodyBudget* budget,
            std::uint64_t streamId,
            bool bodyBudgetBound,
            std::uint64_t actionId) const {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (m_impl->preparedPullId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer has an active prepared pull");
            }
            if (actionId == 0) {
                return Status::InvalidArgument(
                    u"HTTP body producer rejected DATA action id is zero");
            }
            if (m_impl->rejectedPrefixBytes != 0
                && (m_impl->rejectedPrefixStreamId != streamId
                    || actionId >= m_impl->lowestRejectedActionId)) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer rejected DATA would violate replay order");
            }
            return m_impl->queue.ValidateRejectedDataRequeueForAdapter(
                bytes, budget, streamId, bodyBudgetBound);
        }

        Result<void> HttpBodyProducer::PrependRejectedDataForAdapter(
            const std::vector<std::uint8_t>& payload,
            std::uint64_t streamId,
            std::uint64_t actionId) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            if (actionId == 0) {
                return Status::InvalidArgument(
                    u"HTTP body producer rejected DATA action id is zero");
            }
            if (m_impl->rejectedPrefixBytes != 0
                && (m_impl->rejectedPrefixStreamId != streamId
                    || actionId >= m_impl->lowestRejectedActionId)) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body producer rejected DATA would violate replay order");
            }
            auto prepended = m_impl->queue.PrependRejectedDataForAdapter(payload);
            if (!prepended.IsOk()) return prepended;
            if (m_impl->rejectedPrefixBytes == 0) {
                m_impl->rejectedPrefixStreamId = streamId;
            }
            m_impl->rejectedPrefixBytes += payload.size();
            m_impl->lowestRejectedActionId = actionId;
            return {};
        }

        void HttpBodyProducer::FinishRejectedDataRequeueForAdapter() noexcept {
            if (m_impl) m_impl->queue.FinishRejectedDataRequeueForAdapter();
        }

        Result<void> HttpBodyProducer::Close() {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            auto result = m_impl->queue.Close();
            m_impl->SynchronizePreparedPull();
            return result;
        }

        void HttpBodyProducer::Pause() noexcept {
            if (m_impl) m_impl->queue.Pause();
        }

        void HttpBodyProducer::Resume() noexcept {
            if (m_impl) m_impl->queue.Resume();
        }

        void HttpBodyProducer::Cancel() noexcept {
            Cancel(HttpBodyCancelReason::Application);
        }

        void HttpBodyProducer::Cancel(HttpBodyCancelReason reason) noexcept {
            if (m_impl) {
                m_impl->queue.Cancel(reason);
                m_impl->ClearPreparedPull();
                m_impl->SynchronizeRejectedPrefix();
            }
        }

        void HttpBodyProducer::Reset() noexcept {
            if (m_impl) {
                m_impl->queue.Reset();
                m_impl->ClearPreparedPull();
                m_impl->ClearRejectedPrefix();
            }
        }

        Result<void> HttpBodyProducer::SetDeadline(Time::Deadline deadline) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            auto result = m_impl->queue.SetDeadline(deadline);
            m_impl->SynchronizePreparedPull();
            return result;
        }

        Result<void> HttpBodyProducer::SetIdleTimeout(Time::Duration timeout) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            auto result = m_impl->queue.SetIdleTimeout(timeout);
            m_impl->SynchronizePreparedPull();
            return result;
        }

        Result<void> HttpBodyProducer::CheckTimeout() {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            auto result = m_impl->queue.CheckTimeout();
            m_impl->SynchronizePreparedPull();
            return result;
        }

        void HttpBodyProducer::Touch() noexcept {
            if (m_impl) m_impl->queue.Touch();
        }

        Result<void> HttpBodyProducer::AttachBudget(
            HttpBodyBudget* budget, std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            return m_impl->queue.AttachBudget(budget, streamId);
        }

        bool HttpBodyProducer::HasBudget() const noexcept {
            return m_impl != nullptr && m_impl->queue.HasBudget();
        }

        std::uint64_t HttpBodyProducer::BudgetStreamId() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->queue.BudgetStreamId();
        }

        Result<void> HttpBodyProducer::AttachCancellation(
            HttpBodyCancellation* cancellation) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            return m_impl->queue.AttachCancellation(cancellation);
        }

        bool HttpBodyProducer::HasCancellation() const noexcept {
            return m_impl != nullptr && m_impl->queue.HasCancellation();
        }

        Result<void> HttpBodyProducer::AttachBackpressure(
            HttpBodyBackpressureAdapter* adapter) {
            if (!m_impl) return Status::Internal(u"HTTP body producer is moved-from");
            return m_impl->queue.AttachBackpressure(adapter);
        }

        bool HttpBodyProducer::HasBackpressure() const noexcept {
            return m_impl != nullptr && m_impl->queue.HasBackpressure();
        }

        HttpBodyProducerState HttpBodyProducer::State() const noexcept {
            return m_impl == nullptr
                ? HttpBodyProducerState::Failed
                : m_impl->queue.State();
        }

        bool HttpBodyProducer::IsPaused() const noexcept {
            return m_impl != nullptr && m_impl->queue.IsPaused();
        }

        bool HttpBodyProducer::IsCancelled() const noexcept {
            return m_impl != nullptr && m_impl->queue.IsCancelled();
        }

        bool HttpBodyProducer::IsClosed() const noexcept {
            return m_impl != nullptr && m_impl->queue.IsClosed();
        }

        bool HttpBodyProducer::IsExpired() const noexcept {
            return m_impl == nullptr || m_impl->queue.IsExpired();
        }

        bool HttpBodyProducer::NeedsPause() const noexcept {
            return m_impl != nullptr && m_impl->queue.NeedsPause();
        }

        bool HttpBodyProducer::CanResume() const noexcept {
            return m_impl != nullptr && m_impl->queue.CanResume();
        }

        std::size_t HttpBodyProducer::BufferedBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->queue.BufferedBytes();
        }

        std::size_t HttpBodyProducer::WritableBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->queue.WritableBytes();
        }

        std::size_t HttpBodyProducer::MaxBufferedBytes() const noexcept {
            return m_impl == nullptr ? 0 : m_impl->queue.MaxBufferedBytes();
        }

        Status HttpBodyProducer::LastError() const {
            return m_impl == nullptr
                ? Status::Internal(u"HTTP body producer is moved-from")
                : m_impl->queue.LastError();
        }
    }
}
