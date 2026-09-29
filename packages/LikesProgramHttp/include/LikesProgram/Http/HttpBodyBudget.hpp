#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Http {
        struct HttpBodyBudgetLimits {
            std::size_t maxConnectionBytes = 256 * 1024;
            std::size_t maxStreamBytes = 64 * 1024;
        };

        struct HttpBodyBudgetWatermarks {
            std::size_t connectionLowBytes = 0;
            std::size_t connectionHighBytes = 0;
            std::size_t streamLowBytes = 0;
            std::size_t streamHighBytes = 0;
        };

        // Non-owning connection/stream watermark observer. The adapter owns
        // scheduling and decides how these actions affect its transport.
        class HttpBodyBudgetObserver {
        public:
            virtual ~HttpBodyBudgetObserver() = default;

            virtual void PauseConnection() noexcept = 0;
            virtual void ResumeConnection() noexcept = 0;
            virtual void PauseStream(std::uint64_t streamId) noexcept = 0;
            virtual void ResumeStream(std::uint64_t streamId) noexcept = 0;
        };

        struct HttpBodyBudgetSnapshot {
            std::size_t maxConnectionBytes = 0;
            std::size_t maxStreamBytes = 0;
            std::size_t reservedBytes = 0;
            std::size_t activeStreams = 0;
        };

        // Non-owning, adapter-serialized queued-byte accounting shared by
        // body sinks and producers on one logical connection.
        class HttpBodyBudget {
        public:
            LIKESPROGRAM_HTTP_API explicit HttpBodyBudget(
                HttpBodyBudgetLimits limits = {});
            LIKESPROGRAM_HTTP_API HttpBodyBudget(
                HttpBodyBudgetLimits limits,
                HttpBodyBudgetWatermarks watermarks);
            LIKESPROGRAM_HTTP_API ~HttpBodyBudget();

            LIKESPROGRAM_HTTP_API HttpBodyBudget(HttpBodyBudget&&) noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyBudget& operator=(HttpBodyBudget&&) noexcept;
            HttpBodyBudget(const HttpBodyBudget&) = delete;
            HttpBodyBudget& operator=(const HttpBodyBudget&) = delete;

            // Reserve returns the accepted count, allowing callers to apply
            // ordinary partial-acceptance backpressure.
            LIKESPROGRAM_HTTP_API Result<std::size_t> Reserve(
                std::uint64_t streamId,
                std::size_t bytes);
            LIKESPROGRAM_HTTP_API Result<void> Release(
                std::uint64_t streamId,
                std::size_t bytes);
            // Reset is allowed only after all stream reservations are released.
            LIKESPROGRAM_HTTP_API Result<void> Reset();

            // The observer is non-owning and must attach before reservations.
            LIKESPROGRAM_HTTP_API Result<void> AttachObserver(
                HttpBodyBudgetObserver* observer);
            LIKESPROGRAM_HTTP_API bool HasObserver() const noexcept;

            LIKESPROGRAM_HTTP_API std::size_t ReservedBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t ReservedBytes(
                std::uint64_t streamId) const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t AvailableBytes(
                std::uint64_t streamId) const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t MaxConnectionBytes() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t MaxStreamBytes() const noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyBudgetWatermarks Watermarks() const noexcept;
            LIKESPROGRAM_HTTP_API bool NeedsPause(
                std::uint64_t streamId) const noexcept;
            LIKESPROGRAM_HTTP_API bool CanResume(
                std::uint64_t streamId) const noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyBudgetSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
