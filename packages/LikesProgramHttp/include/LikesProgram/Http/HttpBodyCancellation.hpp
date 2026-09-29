#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstdint>

namespace LikesProgram {
    namespace Http {
        enum class HttpBodyCancelReason : std::uint8_t {
            Application,
            PeerReset,
            DeadlineExceeded,
            ProtocolError
        };

        // Non-owning adapter boundary. Implementations map these actions to
        // version-specific FIN/RST/STOP_SENDING or connection operations.
        class HttpBodyCancellationAdapter {
        public:
            virtual ~HttpBodyCancellationAdapter() = default;
            virtual void Cancel(HttpBodyCancelReason reason) noexcept = 0;
            virtual void Reset() noexcept = 0;
        };

        enum class HttpBodyCancellationState : std::uint8_t {
            Open,
            Cancelled
        };

        // A one-shot cancellation state shared by a body handoff and adapter.
        class HttpBodyCancellation {
        public:
            LIKESPROGRAM_HTTP_API explicit HttpBodyCancellation(
                HttpBodyCancellationAdapter* adapter = nullptr) noexcept;

            LIKESPROGRAM_HTTP_API Result<void> AttachAdapter(
                HttpBodyCancellationAdapter* adapter) noexcept;
            LIKESPROGRAM_HTTP_API void Cancel(
                HttpBodyCancelReason reason = HttpBodyCancelReason::Application) noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

            LIKESPROGRAM_HTTP_API HttpBodyCancellationState State() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsCancelled() const noexcept;
            LIKESPROGRAM_HTTP_API bool HasAdapter() const noexcept;
            LIKESPROGRAM_HTTP_API HttpBodyCancelReason Reason() const noexcept;

        private:
            HttpBodyCancellationAdapter* m_adapter = nullptr;
            HttpBodyCancellationState m_state = HttpBodyCancellationState::Open;
            HttpBodyCancelReason m_reason = HttpBodyCancelReason::Application;
        };
    }
}
