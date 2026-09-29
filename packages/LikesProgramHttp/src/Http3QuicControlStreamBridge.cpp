#include <LikesProgram/Http/Http3QuicControlStreamBridge.hpp>

namespace LikesProgram {
    namespace Http {
        struct Http3QuicControlStreamBridge::Impl {
            Http3ControlStreamWireDecoder decoder;
            bool peerReset = false;
            bool peerStopSending = false;
            std::uint64_t transportErrorCode = 0;
            Status failure;

            Impl(std::size_t maxSettings, Http3ControlStreamWireLimits wireLimits)
                : decoder(maxSettings, wireLimits) {}
        };

        Http3QuicControlStreamBridge::Http3QuicControlStreamBridge(
            std::size_t maxSettings,
            Http3ControlStreamWireLimits wireLimits)
            : m_impl(std::make_unique<Impl>(maxSettings, wireLimits)) {}

        Http3QuicControlStreamBridge::~Http3QuicControlStreamBridge() = default;
        Http3QuicControlStreamBridge::Http3QuicControlStreamBridge(
            Http3QuicControlStreamBridge&&) noexcept = default;
        Http3QuicControlStreamBridge& Http3QuicControlStreamBridge::operator=(
            Http3QuicControlStreamBridge&&) noexcept = default;

        Result<void> Http3QuicControlStreamBridge::Feed(
            const Http3QuicEvent& event) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC control stream bridge is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC control stream bridge is transport-terminal");
            }
            Result<void> result;
            switch (event.kind) {
            case Http3QuicEventKind::StreamData:
                result = m_impl->decoder.Feed(event.payload, false);
                break;
            case Http3QuicEventKind::StreamFin:
                result = m_impl->decoder.Feed(event.payload, true);
                break;
            case Http3QuicEventKind::StreamReset:
                m_impl->peerReset = true;
                m_impl->transportErrorCode = event.errorCode;
                return {};
            case Http3QuicEventKind::StopSending:
                m_impl->peerStopSending = true;
                m_impl->transportErrorCode = event.errorCode;
                return {};
            default:
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC control stream bridge event kind is unsupported");
            }
            if (!result.IsOk()) m_impl->failure = result.GetStatus();
            return result;
        }

        Http3QuicControlStreamBridgeSnapshot
            Http3QuicControlStreamBridge::Snapshot() const noexcept {
            if (!m_impl) return {};
            return { m_impl->decoder.Snapshot(), m_impl->peerReset,
                m_impl->peerStopSending, m_impl->transportErrorCode };
        }

        Result<Http3ControlStreamQuicActions>
            Http3QuicControlStreamBridge::FailureActions() const {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC control stream bridge is moved-from");
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Http3ControlStreamQuicActions{};
            }
            return m_impl->decoder.FailureActions();
        }

        Status Http3QuicControlStreamBridge::LastError() const {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC control stream bridge is moved-from");
            return m_impl->failure.IsOk() ? m_impl->decoder.LastError()
                : m_impl->failure;
        }

        void Http3QuicControlStreamBridge::Reset() noexcept {
            if (!m_impl) return;
            m_impl->decoder.Reset();
            m_impl->peerReset = false;
            m_impl->peerStopSending = false;
            m_impl->transportErrorCode = 0;
            m_impl->failure = {};
        }
    }
}
