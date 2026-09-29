#include <LikesProgram/Http/HttpBodyCancellation.hpp>

namespace LikesProgram {
    namespace Http {
        HttpBodyCancellation::HttpBodyCancellation(
            HttpBodyCancellationAdapter* adapter) noexcept
            : m_adapter(adapter) { }

        Result<void> HttpBodyCancellation::AttachAdapter(
            HttpBodyCancellationAdapter* adapter) noexcept {
            if (adapter == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP body cancellation adapter is null");
            }
            if (m_state == HttpBodyCancellationState::Cancelled) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body cancellation is already cancelled");
            }
            if (m_adapter != nullptr && m_adapter != adapter) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP body cancellation already has an adapter");
            }
            m_adapter = adapter;
            return {};
        }

        void HttpBodyCancellation::Cancel(HttpBodyCancelReason reason) noexcept {
            if (m_state == HttpBodyCancellationState::Cancelled) return;
            m_state = HttpBodyCancellationState::Cancelled;
            m_reason = reason;
            if (m_adapter != nullptr) m_adapter->Cancel(reason);
        }

        void HttpBodyCancellation::Reset() noexcept {
            const auto wasCancelled = m_state == HttpBodyCancellationState::Cancelled;
            m_state = HttpBodyCancellationState::Open;
            m_reason = HttpBodyCancelReason::Application;
            if (wasCancelled && m_adapter != nullptr) m_adapter->Reset();
        }

        HttpBodyCancellationState HttpBodyCancellation::State() const noexcept {
            return m_state;
        }

        bool HttpBodyCancellation::IsCancelled() const noexcept {
            return m_state == HttpBodyCancellationState::Cancelled;
        }

        bool HttpBodyCancellation::HasAdapter() const noexcept {
            return m_adapter != nullptr;
        }

        HttpBodyCancelReason HttpBodyCancellation::Reason() const noexcept {
            return m_reason;
        }
    }
}
