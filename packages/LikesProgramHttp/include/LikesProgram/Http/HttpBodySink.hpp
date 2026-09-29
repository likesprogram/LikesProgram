#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpBodyBackpressure.hpp>
#include <LikesProgram/Http/HttpBodyCancellation.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Http {
        class HttpBodyBudget;
        class HttpBodyProducer;

        struct HttpBodyDrainEvent {
            std::uint64_t streamId = 0;
            std::size_t bytes = 0;
            std::uint64_t totalPulledBytes = 0;
            std::size_t remainingBufferedBytes = 0;
        };

        // Non-owning synchronous notification after a public application Pull
        // commits. Implementations may schedule transport work, but must not
        // mutatingly reenter, move or destroy the participating sink.
        class HttpBodyDrainObserver {
        public:
            virtual ~HttpBodyDrainObserver() = default;
            virtual void Observe(const HttpBodyDrainEvent& event) noexcept = 0;
        };

        struct HttpBodySinkLimits {
            std::size_t maxBufferedBytes = 64 * 1024;
            std::size_t lowWatermark = 16 * 1024;
            std::size_t highWatermark = 64 * 1024;
        };

        enum class HttpBodySinkState : std::uint8_t {
            Open,
            Paused,
            Closed,
            Cancelled,
            Expired,
            Failed
        };

        // Sans-I/O bounded body handoff. The caller owns scheduling and transport.
        class HttpBodySink {
        public:
            LIKESPROGRAM_HTTP_API explicit HttpBodySink(HttpBodySinkLimits limits = {});
            LIKESPROGRAM_HTTP_API ~HttpBodySink();

            LIKESPROGRAM_HTTP_API HttpBodySink(HttpBodySink&&) noexcept;
            LIKESPROGRAM_HTTP_API HttpBodySink& operator=(HttpBodySink&&) noexcept;
            HttpBodySink(const HttpBodySink&) = delete;
            HttpBodySink& operator=(const HttpBodySink&) = delete;

            // Push accepts as much as the bounded queue can hold and returns that count.
            LIKESPROGRAM_HTTP_API Result<std::size_t> Push(
                const std::uint8_t* data,
                std::size_t size);
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> Pull(
                std::size_t maxBytes);

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

            // The budget is non-owning and must outlive this sink. A sink may
            // attach only before it has buffered bytes.
            LIKESPROGRAM_HTTP_API Result<void> AttachBudget(
                HttpBodyBudget* budget,
                std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API bool HasBudget() const noexcept;
            LIKESPROGRAM_HTTP_API std::uint64_t BudgetStreamId() const noexcept;

            // The cancellation bridge is non-owning and must outlive this sink.
            LIKESPROGRAM_HTTP_API Result<void> AttachCancellation(
                HttpBodyCancellation* cancellation);
            LIKESPROGRAM_HTTP_API bool HasCancellation() const noexcept;

            // The backpressure adapter is non-owning and must outlive this
            // sink. Attach it before buffering so the first high watermark
            // transition is observable.
            LIKESPROGRAM_HTTP_API Result<void> AttachBackpressure(
                HttpBodyBackpressureAdapter* adapter);
            LIKESPROGRAM_HTTP_API bool HasBackpressure() const noexcept;

            // The drain observer is non-owning and must outlive this sink.
            // It observes subsequent successful, non-empty public Pull calls.
            LIKESPROGRAM_HTTP_API Result<void> AttachDrainObserver(
                HttpBodyDrainObserver* observer,
                std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API bool HasDrainObserver() const noexcept;
            LIKESPROGRAM_HTTP_API std::uint64_t
                DrainObserverStreamId() const noexcept;

            LIKESPROGRAM_HTTP_API HttpBodySinkState State() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsPaused() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsCancelled() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsClosed() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsExpired() const noexcept;
            LIKESPROGRAM_HTTP_API bool NeedsPause() const noexcept;
            LIKESPROGRAM_HTTP_API bool CanResume() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t BufferedBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t WritableBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t MaxBufferedBytes() const noexcept;
            // Monotonically counts bytes returned by successful public Pull
            // calls. Reset and internal producer/adapter transfers are excluded.
            LIKESPROGRAM_HTTP_API std::uint64_t PulledBytes() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;

        private:
            friend class HttpBodyProducer;

            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>>
                PeekForProducer(std::size_t maxBytes);
            LIKESPROGRAM_HTTP_API Result<void> ConsumeForProducer(
                std::size_t bytes);
            LIKESPROGRAM_HTTP_API Result<bool> CanTransferBudgetToAdapter(
                HttpBodyBudget* budget,
                std::uint64_t streamId) const;
            LIKESPROGRAM_HTTP_API void ConsumeForAdapter(
                std::size_t bytes,
                bool releaseBudget) noexcept;
            LIKESPROGRAM_HTTP_API Result<bool>
                ValidateRejectedDataRequeueForAdapter(
                    std::size_t bytes,
                    HttpBodyBudget* budget,
                    std::uint64_t streamId,
                    bool bodyBudgetBound) const;
            LIKESPROGRAM_HTTP_API Result<void>
                PrependRejectedDataForAdapter(
                    const std::vector<std::uint8_t>& payload);
            LIKESPROGRAM_HTTP_API void
                FinishRejectedDataRequeueForAdapter() noexcept;

            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
