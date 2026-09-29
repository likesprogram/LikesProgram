#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Http {
        class Http3QuicAdapter;

        // Producer-side names make the direction explicit while sharing the
        // bounded queue limits and state values with HttpBodySink.
        using HttpBodyProducerLimits = HttpBodySinkLimits;
        using HttpBodyProducerState = HttpBodySinkState;

        struct HttpBodyProducerPreparedPull {
            bool available = false;
            std::uint64_t id = 0;
            std::vector<std::uint8_t> payload;
        };

        struct HttpBodyRejectedDataReplaySnapshot {
            bool available = false;
            std::uint64_t streamId = 0;
            std::uint64_t lowestActionId = 0;
            std::size_t bytes = 0;
        };

        // Sans-I/O outbound body handoff. The application pushes bytes and a
        // transport adapter pulls them; neither side is owned by this class.
        class HttpBodyProducer {
        public:
            LIKESPROGRAM_HTTP_API explicit HttpBodyProducer(
                HttpBodyProducerLimits limits = {});
            LIKESPROGRAM_HTTP_API ~HttpBodyProducer();

            LIKESPROGRAM_HTTP_API HttpBodyProducer(HttpBodyProducer&&) noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyProducer& operator=(HttpBodyProducer&&) noexcept;
            HttpBodyProducer(const HttpBodyProducer&) = delete;
            HttpBodyProducer& operator=(const HttpBodyProducer&) = delete;

            // Push accepts as much as the bounded queue can hold.
            LIKESPROGRAM_HTTP_API Result<std::size_t> Push(
                const std::uint8_t* data,
                std::size_t size);
            // Pull returns up to maxBytes for the transport adapter.
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> Pull(
                std::size_t maxBytes);
            // Prepare keeps the copied prefix and its budget reservation in
            // the queue until the caller commits or rolls back the id.
            LIKESPROGRAM_HTTP_API Result<HttpBodyProducerPreparedPull> PreparePull(
                std::size_t maxBytes);
            // Prepares exactly the active rejected-DATA prefix without
            // including later application bytes.
            LIKESPROGRAM_HTTP_API Result<HttpBodyProducerPreparedPull>
                PrepareRejectedDataReplay();
            LIKESPROGRAM_HTTP_API Result<void> CommitPull(std::uint64_t id);
            LIKESPROGRAM_HTTP_API Result<void> RollbackPull(std::uint64_t id);
            LIKESPROGRAM_HTTP_API bool HasPreparedPull() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t PreparedPullBytes() const noexcept;
            LIKESPROGRAM_HTTP_API bool HasRejectedDataReplay() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t
                RejectedDataReplayBytes() const noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyRejectedDataReplaySnapshot
                RejectedDataReplay() const noexcept;

            // Close stops new application writes but retains buffered bytes.
            LIKESPROGRAM_HTTP_API Result<void> Close();
            LIKESPROGRAM_HTTP_API void Pause() noexcept;
            LIKESPROGRAM_HTTP_API void Resume() noexcept;
            LIKESPROGRAM_HTTP_API void Cancel() noexcept;
            LIKESPROGRAM_HTTP_API void Cancel(HttpBodyCancelReason reason) noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

            LIKESPROGRAM_HTTP_API Result<void> SetDeadline(Time::Deadline deadline);
            LIKESPROGRAM_HTTP_API Result<void> SetIdleTimeout(Time::Duration timeout);
            LIKESPROGRAM_HTTP_API Result<void> CheckTimeout();
            LIKESPROGRAM_HTTP_API void Touch() noexcept;

            // The budget is non-owning and must outlive this producer.
            LIKESPROGRAM_HTTP_API Result<void> AttachBudget(
                HttpBodyBudget* budget,
                std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API bool HasBudget() const noexcept;
            LIKESPROGRAM_HTTP_API std::uint64_t BudgetStreamId() const noexcept;

            LIKESPROGRAM_HTTP_API Result<void> AttachCancellation(
                HttpBodyCancellation* cancellation);
            LIKESPROGRAM_HTTP_API bool HasCancellation() const noexcept;

            // The backpressure adapter is non-owning and must outlive this
            // producer. Attach it before buffering so the first high
            // watermark transition is observable.
            LIKESPROGRAM_HTTP_API Result<void> AttachBackpressure(
                HttpBodyBackpressureAdapter* adapter);
            LIKESPROGRAM_HTTP_API bool HasBackpressure() const noexcept;

            LIKESPROGRAM_HTTP_API HttpBodyProducerState State() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsPaused() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsCancelled() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsClosed() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsExpired() const noexcept;
            LIKESPROGRAM_HTTP_API bool NeedsPause() const noexcept;
            LIKESPROGRAM_HTTP_API bool CanResume() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t BufferedBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t WritableBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t MaxBufferedBytes() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;

        private:
            friend class Http3QuicAdapter;

            LIKESPROGRAM_HTTP_API Result<bool> ValidatePreparedPullForAdapter(
                std::uint64_t id,
                HttpBodyBudget* budget,
                std::uint64_t streamId) const;
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>>
                CopyPreparedPullForAdapter(std::uint64_t id);
            LIKESPROGRAM_HTTP_API void CommitPreparedPullForAdapter(
                bool transferBudget) noexcept;
            LIKESPROGRAM_HTTP_API Result<bool>
                ValidateRejectedDataRequeueForAdapter(
                    std::size_t bytes,
                    HttpBodyBudget* budget,
                    std::uint64_t streamId,
                    bool bodyBudgetBound,
                    std::uint64_t actionId) const;
            LIKESPROGRAM_HTTP_API Result<void>
                PrependRejectedDataForAdapter(
                    const std::vector<std::uint8_t>& payload,
                    std::uint64_t streamId,
                    std::uint64_t actionId);
            LIKESPROGRAM_HTTP_API void
                FinishRejectedDataRequeueForAdapter() noexcept;

            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
