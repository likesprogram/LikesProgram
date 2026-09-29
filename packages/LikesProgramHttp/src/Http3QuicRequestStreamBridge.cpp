#include <LikesProgram/Http/Http3QuicRequestStreamBridge.hpp>

#include <limits>
#include <unordered_map>

namespace LikesProgram {
    namespace Http {
        namespace {
            constexpr std::uint64_t MaxQuicVarInt =
                (std::uint64_t{ 1 } << 62) - 1;
        }

        struct Http3ConnectionReceiveCreditCoordinator::Impl {
            bool configured = false;
            std::uint64_t initialLimit = 0;
            std::uint64_t aggregatePulledBytes = 0;
            std::unordered_map<std::uint64_t, std::uint64_t> streams;
        };

        Http3ConnectionReceiveCreditCoordinator::
            Http3ConnectionReceiveCreditCoordinator()
            : m_impl(std::make_unique<Impl>()) {}

        Http3ConnectionReceiveCreditCoordinator::
            ~Http3ConnectionReceiveCreditCoordinator() = default;
        Http3ConnectionReceiveCreditCoordinator::
            Http3ConnectionReceiveCreditCoordinator(
                Http3ConnectionReceiveCreditCoordinator&&) noexcept = default;
        Http3ConnectionReceiveCreditCoordinator&
            Http3ConnectionReceiveCreditCoordinator::operator=(
                Http3ConnectionReceiveCreditCoordinator&&) noexcept = default;

        Result<void> Http3ConnectionReceiveCreditCoordinator::Configure(
            std::uint64_t initialLimit) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 connection receive-credit coordinator is moved-from");
            if (initialLimit == 0 || initialLimit > MaxQuicVarInt) {
                return Status::InvalidArgument(
                    u"HTTP/3 connection receive-credit initial limit is invalid");
            }
            if (m_impl->configured) {
                if (m_impl->initialLimit == initialLimit) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit initial limit conflicts");
            }
            m_impl->configured = true;
            m_impl->initialLimit = initialLimit;
            return {};
        }

        Result<void>
            Http3ConnectionReceiveCreditCoordinator::RegisterRequestStream(
                const Http3QuicRequestStreamBridgeSnapshot& snapshot) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 connection receive-credit coordinator is moved-from");
            if (!m_impl->configured) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit coordinator is not configured");
            }
            if (!snapshot.hasBodySink) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit stream has no body sink");
            }
            const auto streamId = snapshot.stream.streamId;
            if (streamId > MaxQuicVarInt) {
                return Status::InvalidArgument(
                    u"HTTP/3 connection receive-credit stream id exceeds 62 bits");
            }
            const auto existing = m_impl->streams.find(streamId);
            if (existing != m_impl->streams.end()) {
                if (existing->second == snapshot.bodyPulledBytes) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit stream registration conflicts");
            }
            try {
                m_impl->streams.emplace(streamId, snapshot.bodyPulledBytes);
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 connection receive-credit stream registration failed");
            }
            return {};
        }

        Result<bool>
            Http3ConnectionReceiveCreditCoordinator::ObserveRequestStream(
                const Http3QuicRequestStreamBridgeSnapshot& snapshot) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 connection receive-credit coordinator is moved-from");
            if (!m_impl->configured) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit coordinator is not configured");
            }
            if (!snapshot.hasBodySink) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit stream has no body sink");
            }
            const auto streamId = snapshot.stream.streamId;
            const auto stream = m_impl->streams.find(streamId);
            if (stream == m_impl->streams.end()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit stream is not registered");
            }
            if (snapshot.bodyPulledBytes < stream->second) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit pulled-byte counter regressed");
            }
            const auto delta = snapshot.bodyPulledBytes - stream->second;
            if (delta == 0) return false;
            const auto remaining = MaxQuicVarInt - m_impl->initialLimit
                - m_impl->aggregatePulledBytes;
            if (delta > remaining) {
                return Status::InvalidArgument(
                    u"HTTP/3 connection receive-credit aggregate exceeds 62 bits");
            }
            stream->second = snapshot.bodyPulledBytes;
            m_impl->aggregatePulledBytes += delta;
            return true;
        }

        Result<void>
            Http3ConnectionReceiveCreditCoordinator::UnregisterRequestStream(
                std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 connection receive-credit coordinator is moved-from");
            if (m_impl->streams.erase(streamId) == 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit stream is not registered");
            }
            return {};
        }

        Result<bool> Http3ConnectionReceiveCreditCoordinator::
            RefreshConnectionReceiveCredit(Http3QuicAdapter& adapter) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 connection receive-credit coordinator is moved-from");
            if (!m_impl->configured) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 connection receive-credit coordinator is not configured");
            }
            if (m_impl->aggregatePulledBytes == 0) return false;
            const auto target = m_impl->initialLimit
                + m_impl->aggregatePulledBytes;
            const auto credit = adapter.ReceiveCredit(0);
            if (!credit.IsOk()) return credit.GetStatus();
            if (credit.Value().connectionUpdatePending
                || (credit.Value().connectionCreditSet
                    && credit.Value().connectionCredit >= target)) {
                return false;
            }
            auto updated = adapter.UpdateConnectionReceiveCredit(target);
            if (!updated.IsOk()) return updated.GetStatus();
            return true;
        }

        Http3ConnectionReceiveCreditCoordinatorSnapshot
            Http3ConnectionReceiveCreditCoordinator::Snapshot() const noexcept {
            if (!m_impl) return {};
            const bool targetValid = m_impl->configured
                && m_impl->aggregatePulledBytes
                    <= MaxQuicVarInt - m_impl->initialLimit;
            return { m_impl->configured, m_impl->initialLimit,
                m_impl->streams.size(), m_impl->aggregatePulledBytes,
                targetValid
                    ? m_impl->initialLimit + m_impl->aggregatePulledBytes : 0,
                targetValid };
        }

        void Http3ConnectionReceiveCreditCoordinator::Reset() noexcept {
            if (!m_impl) return;
            m_impl->configured = false;
            m_impl->initialLimit = 0;
            m_impl->aggregatePulledBytes = 0;
            m_impl->streams.clear();
        }

        struct Http3QuicRequestStreamBridge::Impl {
            Http3RequestStreamWireDecoder decoder;
            std::uint64_t streamId = 0;
            bool peerReset = false;
            bool peerStopSending = false;
            std::uint64_t transportErrorCode = 0;
            Status failure;
            HttpBodySink* bodySink = nullptr;
            std::uint64_t bodyPulledBaseline = 0;
            bool streamReceiveCreditConfigured = false;
            std::uint64_t streamReceiveCreditBaseLimit = 0;

            Impl(std::uint64_t id, Http3StreamBodyLimits bodyLimits,
                Http3RequestStreamWireLimits wireLimits)
                : decoder(id, bodyLimits, wireLimits), streamId(id) {}
        };

        Http3QuicRequestStreamBridge::Http3QuicRequestStreamBridge(
            std::uint64_t streamId,
            Http3StreamBodyLimits bodyLimits,
            Http3RequestStreamWireLimits wireLimits)
            : m_impl(std::make_unique<Impl>(streamId, bodyLimits, wireLimits)) {}

        Http3QuicRequestStreamBridge::~Http3QuicRequestStreamBridge() = default;
        Http3QuicRequestStreamBridge::Http3QuicRequestStreamBridge(
            Http3QuicRequestStreamBridge&&) noexcept = default;
        Http3QuicRequestStreamBridge& Http3QuicRequestStreamBridge::operator=(
            Http3QuicRequestStreamBridge&&) noexcept = default;

        Result<void> Http3QuicRequestStreamBridge::Feed(
            const Http3QuicEvent& event) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (event.streamId != m_impl->streamId) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC request stream bridge stream id mismatch");
            }
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge is transport-terminal");
            }
            if (m_impl->decoder.HasPendingBodyRetry()
                && (event.kind == Http3QuicEventKind::StreamData
                    || event.kind == Http3QuicEventKind::StreamFin)) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge requires an explicit body retry");
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
                if (m_impl->bodySink != nullptr) {
                    m_impl->bodySink->Cancel(HttpBodyCancelReason::PeerReset);
                }
                return {};
            case Http3QuicEventKind::StopSending:
                m_impl->peerStopSending = true;
                m_impl->transportErrorCode = event.errorCode;
                return {};
            default:
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC request stream bridge event kind is unsupported");
            }
            if (!result.IsOk()
                && !(result.GetStatus().Code() == StatusCode::ResourceExhausted
                    && m_impl->decoder.HasPendingBodyRetry())) {
                m_impl->failure = result.GetStatus();
            }
            return result;
        }

        Result<void> Http3QuicRequestStreamBridge::AttachBodySink(
            HttpBodySink* sink) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (sink == nullptr) return Status::InvalidArgument(
                u"HTTP/3 QUIC request stream bridge body sink is null");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge is transport-terminal");
            }
            if (m_impl->bodySink != nullptr) {
                if (m_impl->bodySink == sink) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge already has a body sink");
            }
            auto attached = m_impl->decoder.AttachBodySink(sink);
            if (!attached.IsOk()) return attached;
            m_impl->bodySink = sink;
            m_impl->bodyPulledBaseline = sink->PulledBytes();
            return {};
        }

        bool Http3QuicRequestStreamBridge::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->bodySink != nullptr;
        }

        Result<void> Http3QuicRequestStreamBridge::RetryPendingBody() {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge is transport-terminal");
            }
            auto retried = m_impl->decoder.RetryPendingBody();
            if (!retried.IsOk()
                && !(retried.GetStatus().Code() == StatusCode::ResourceExhausted
                    && m_impl->decoder.HasPendingBodyRetry())) {
                m_impl->failure = retried.GetStatus();
            }
            return retried;
        }

        Result<void> Http3QuicRequestStreamBridge::ConfigureStreamReceiveCredit(
            std::uint64_t initialLimit) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge is transport-terminal");
            }
            if (m_impl->bodySink == nullptr) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge has no body sink");
            }
            if (initialLimit == 0 || initialLimit > MaxQuicVarInt) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC request stream receive-credit limit is invalid");
            }
            if (m_impl->streamReceiveCreditConfigured) {
                if (m_impl->streamReceiveCreditBaseLimit == initialLimit) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream receive-credit limit conflicts");
            }

            const auto pulled = m_impl->bodySink->PulledBytes();
            if (pulled < m_impl->bodyPulledBaseline
                || pulled - m_impl->bodyPulledBaseline
                    > MaxQuicVarInt - initialLimit) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream pulled-byte baseline is invalid");
            }
            m_impl->streamReceiveCreditConfigured = true;
            m_impl->streamReceiveCreditBaseLimit = initialLimit;
            return {};
        }

        Result<bool> Http3QuicRequestStreamBridge::RefreshStreamReceiveCredit(
            Http3QuicAdapter& adapter) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream bridge is transport-terminal");
            }
            if (m_impl->bodySink == nullptr
                || !m_impl->streamReceiveCreditConfigured) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream receive credit is not configured");
            }

            const auto pulled = m_impl->bodySink->PulledBytes();
            if (pulled < m_impl->bodyPulledBaseline) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC request stream body sink was reset independently");
            }
            const auto pulledSinceAttach = pulled - m_impl->bodyPulledBaseline;
            if (pulledSinceAttach
                > MaxQuicVarInt - m_impl->streamReceiveCreditBaseLimit) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC request stream receive-credit target exceeds 62 bits");
            }
            if (pulledSinceAttach == 0) return false;
            const auto target = m_impl->streamReceiveCreditBaseLimit
                + pulledSinceAttach;

            const auto credit = adapter.ReceiveCredit(m_impl->streamId);
            if (!credit.IsOk()) return credit.GetStatus();
            if (credit.Value().streamUpdatePending
                || (credit.Value().streamCreditSet
                    && credit.Value().streamCredit >= target)) {
                return false;
            }
            auto updated = adapter.UpdateStreamReceiveCredit(
                m_impl->streamId, target);
            if (!updated.IsOk()) return updated.GetStatus();
            return true;
        }

        Http3QuicRequestStreamBridgeSnapshot
            Http3QuicRequestStreamBridge::Snapshot() const noexcept {
            if (!m_impl) return {};
            const auto pendingBodyBytes = m_impl->decoder.PendingBodyBytes();
            const auto bodyBufferedBytes = m_impl->bodySink == nullptr
                ? 0 : m_impl->bodySink->BufferedBytes();
            const auto bodyWritableBytes = m_impl->bodySink == nullptr
                ? 0 : m_impl->bodySink->WritableBytes();
            const auto bodyCapacityBytes = m_impl->bodySink == nullptr
                ? 0 : m_impl->bodySink->MaxBufferedBytes();
            const bool bodyRetryReady = m_impl->bodySink != nullptr
                && m_impl->decoder.HasPendingBodyRetry()
                && m_impl->bodySink->State() == HttpBodySinkState::Open
                && bodyWritableBytes >= pendingBodyBytes;
            const auto bodyPulledBytes = m_impl->bodySink == nullptr
                ? 0 : m_impl->bodySink->PulledBytes();
            const bool bodyPulledBaselineValid = m_impl->bodySink != nullptr
                && bodyPulledBytes >= m_impl->bodyPulledBaseline;
            const auto bodyPulledSinceAttach = bodyPulledBaselineValid
                ? bodyPulledBytes - m_impl->bodyPulledBaseline : 0;
            const bool streamReceiveCreditTargetValid =
                m_impl->streamReceiveCreditConfigured
                && bodyPulledBaselineValid
                && bodyPulledSinceAttach <= MaxQuicVarInt
                    - m_impl->streamReceiveCreditBaseLimit;
            const auto streamReceiveCreditTargetLimit =
                streamReceiveCreditTargetValid
                ? m_impl->streamReceiveCreditBaseLimit + bodyPulledSinceAttach
                : 0;
            return { m_impl->decoder.Snapshot(), m_impl->peerReset,
                m_impl->peerStopSending, m_impl->transportErrorCode,
                m_impl->decoder.HasBodySink(),
                m_impl->decoder.HasPendingBodyRetry(),
                m_impl->decoder.PendingBytes(), pendingBodyBytes,
                bodyBufferedBytes, bodyWritableBytes, bodyCapacityBytes,
                bodyRetryReady, bodyPulledBytes, bodyPulledSinceAttach,
                m_impl->streamReceiveCreditConfigured,
                m_impl->streamReceiveCreditBaseLimit,
                streamReceiveCreditTargetLimit,
                streamReceiveCreditTargetValid };
        }

        Result<Http3RequestStreamQuicActions>
            Http3QuicRequestStreamBridge::FailureActions() const {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            if (m_impl->peerReset || m_impl->peerStopSending) {
                return Http3RequestStreamQuicActions{};
            }
            return m_impl->decoder.FailureActions();
        }

        Status Http3QuicRequestStreamBridge::LastError() const {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 QUIC request stream bridge is moved-from");
            return m_impl->failure.IsOk() ? m_impl->decoder.LastError()
                : m_impl->failure;
        }

        void Http3QuicRequestStreamBridge::Reset() noexcept {
            if (!m_impl) return;
            const auto bodyPulledBytes = m_impl->bodySink == nullptr
                ? 0 : m_impl->bodySink->PulledBytes();
            m_impl->decoder.Reset();
            m_impl->peerReset = false;
            m_impl->peerStopSending = false;
            m_impl->transportErrorCode = 0;
            m_impl->failure = {};
            m_impl->bodyPulledBaseline = bodyPulledBytes;
            m_impl->streamReceiveCreditConfigured = false;
            m_impl->streamReceiveCreditBaseLimit = 0;
        }
    }
}
