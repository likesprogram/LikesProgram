#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpObservability.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Http {
        enum class HttpBodyCancelReason : std::uint8_t;
        class HttpBodyBudget;
        class HttpBodyProducer;

        // Events are supplied by an external QUIC implementation after it has
        // removed UDP packet, TLS, and QUIC framing.
        enum class Http3QuicEventKind : std::uint8_t {
            HandshakeComplete,
            StreamData,
            StreamFin,
            StreamReset,
            StopSending,
            ConnectionClose,
            StreamSendCredit,
            ConnectionSendCredit
        };

        struct Http3QuicEvent {
            Http3QuicEvent() = default;
            Http3QuicEvent(Http3QuicEventKind eventKind, std::uint64_t eventStreamId = 0, std::uint64_t eventErrorCode = 0, std::vector<std::uint8_t> eventPayload = {}, std::uint64_t eventFlowCredit = 0, bool eventApplicationError = false)
                : kind(eventKind), streamId(eventStreamId), errorCode(eventErrorCode), payload(std::move(eventPayload)), flowCredit(eventFlowCredit), applicationError(eventApplicationError) { }

            Http3QuicEventKind kind = Http3QuicEventKind::HandshakeComplete;
            std::uint64_t streamId = 0;
            std::uint64_t errorCode = 0;
            std::vector<std::uint8_t> payload;
            // For StreamSendCredit, this is the newly available byte credit.
            std::uint64_t flowCredit = 0;
            // True when CONNECTION_CLOSE carries an application error code.
            bool applicationError = false;
        };

        // Actions are mapped by the external adapter to QUIC stream or
        // connection operations and then to UDP packets.
        enum class Http3QuicActionKind : std::uint8_t {
            StreamData,
            StreamFin,
            ResetStream,
            StopSending,
            CloseConnection,
            StreamReceiveCredit,
            ConnectionReceiveCredit
        };

        struct Http3QuicAction {
            Http3QuicActionKind kind = Http3QuicActionKind::StreamData;
            std::uint64_t streamId = 0;
            std::uint64_t errorCode = 0;
            std::vector<std::uint8_t> payload;
            // Assigned by the adapter for external transport acknowledgement.
            std::uint64_t actionId = 0;
            // Absolute receive limit for StreamReceiveCredit or
            // ConnectionReceiveCredit; zero for all other action kinds.
            std::uint64_t flowCredit = 0;
        };

        // Feedback is supplied by the caller-owned QUIC/UDP adapter after it
        // observes the external send queue. It does not carry socket or QUIC
        // implementation types into the public HTTP contract.
        enum class Http3QuicTransportFeedbackKind : std::uint8_t {
            Writable,
            Blocked,
            ActionAccepted,
            ActionRejected
        };

        struct Http3QuicTransportFeedback {
            Http3QuicTransportFeedbackKind kind = Http3QuicTransportFeedbackKind::Writable;
            std::uint64_t actionId = 0;
            std::uint64_t acceptedBytes = 0;
            std::uint64_t errorCode = 0;
        };

        class Http3QuicActionSink {
        public:
            virtual ~Http3QuicActionSink() = default;
            // Submit is synchronous and caller-serialized. It must not reenter
            // the participating adapter or body producer during submission.
            virtual void Submit(const Http3QuicAction& action) noexcept = 0;
        };

        class Http3QuicEventObserver {
        public:
            virtual ~Http3QuicEventObserver() = default;
            virtual void Observe(const Http3QuicEvent& event) noexcept = 0;
        };

        struct Http3RequestStreamQuicActions;

        enum class Http3QuicAdapterState : std::uint8_t {
            AwaitingHandshake,
            Ready,
            Closing,
            Closed,
            Failed
        };

        struct Http3QuicAdapterSnapshot {
            Http3QuicAdapterState state = Http3QuicAdapterState::AwaitingHandshake;
            bool handshakeComplete = false;
            std::size_t activeStreams = 0;
            std::size_t eventCount = 0;
            std::size_t actionCount = 0;
            std::uint64_t lastErrorCode = 0;
            bool connectionSendCreditActive = false;
            std::uint64_t connectionSendCredit = 0;
            std::size_t trackedStreamCredits = 0;
            std::size_t zeroStreamCredits = 0;
            bool transportBlocked = false;
            std::size_t pendingDataActions = 0;
            std::size_t pendingControlActions = 0;
            std::size_t pendingTerminalActions = 0;
            std::size_t pendingDataBytes = 0;
            std::size_t pendingBodyBudgetBytes = 0;
            std::size_t pendingReceiveCreditActions = 0;
            // Stream slots created by an outbound action but not yet accepted
            // by the external queue or observed from the peer.
            std::size_t provisionalStreams = 0;
        };

        struct Http3QuicSendCreditSnapshot {
            bool connectionCreditActive = false;
            std::uint64_t connectionCredit = 0;
            bool streamCreditActive = false;
            std::uint64_t streamCredit = 0;
            bool bounded = false;
            bool blocked = false;
            std::uint64_t availableCredit = 0;
        };

        struct Http3QuicPendingDataSnapshot {
            std::size_t actions = 0;
            std::size_t bytes = 0;
            std::size_t bodyBudgetBytes = 0;
        };

        struct Http3QuicReceiveCreditSnapshot {
            bool connectionCreditSet = false;
            std::uint64_t connectionCredit = 0;
            bool connectionUpdatePending = false;
            std::uint64_t pendingConnectionCredit = 0;
            bool streamCreditSet = false;
            std::uint64_t streamCredit = 0;
            bool streamUpdatePending = false;
            std::uint64_t pendingStreamCredit = 0;
        };

        struct Http3QuicRejectedData {
            bool available = false;
            std::uint64_t actionId = 0;
            std::uint64_t streamId = 0;
            std::vector<std::uint8_t> payload;
        };

        // Terminal peer/order failures retain the event coordinates needed by
        // a caller-owned diagnostic or transport adapter.
        constexpr std::size_t kHttp3QuicMaxCloseReasonBytes = 256;

        struct Http3QuicErrorContext {
            bool valid = false;
            Http3QuicEventKind eventKind = Http3QuicEventKind::HandshakeComplete;
            std::uint64_t streamId = 0;
            std::uint64_t errorCode = 0;
            // True when the caller-owned connection deadline was observed
            // expired; deadline expiry remains recoverable until the caller
            // supplies a new deadline.
            bool deadlineExpired = false;
            // True when the retained close event was application-level.
            bool applicationError = false;
            std::array<std::uint8_t, kHttp3QuicMaxCloseReasonBytes> closeReason{};
            std::size_t closeReasonSize = 0;
            // Last accepted transport rejection, if any. This is feedback
            // metadata only; it does not make the adapter own QUIC state.
            bool transportFeedbackValid = false;
            Http3QuicTransportFeedbackKind transportFeedbackKind = Http3QuicTransportFeedbackKind::Writable;
            std::uint64_t transportActionId = 0;
            std::uint64_t transportErrorCode = 0;
        };

        struct Http3QuicAdapterLimits {
            std::size_t maxActiveStreams = 100;
            std::size_t maxEvents = 65536;
            std::size_t maxActions = 65536;
            std::size_t maxPayloadBytes = 16 * 1024;
        };

        // Serialized, non-owning HTTP/3 over QUIC handoff. This class does not
        // parse UDP packets or own a socket, TLS engine, timer, or QUIC state.
        class Http3QuicAdapter {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QuicAdapter(Http3QuicActionSink* actionSink = nullptr, Http3QuicEventObserver* eventObserver = nullptr);
            LIKESPROGRAM_HTTP_API ~Http3QuicAdapter();

            LIKESPROGRAM_HTTP_API Http3QuicAdapter(Http3QuicAdapter&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QuicAdapter& operator=(Http3QuicAdapter&&) noexcept;
            Http3QuicAdapter(const Http3QuicAdapter&) = delete;
            Http3QuicAdapter& operator=(const Http3QuicAdapter&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> AttachActionSink(Http3QuicActionSink* sink) noexcept;
            LIKESPROGRAM_HTTP_API Result<void> AttachEventObserver(Http3QuicEventObserver* observer) noexcept;
            // The budget is non-owning and must outlive this adapter. While
            // attached, the adapter exclusively owns reservations created for
            // feedback-pending DATA actions.
            LIKESPROGRAM_HTTP_API Result<void> AttachBodyBudget(HttpBodyBudget* budget);
            LIKESPROGRAM_HTTP_API bool HasBodyBudget() const noexcept;
            LIKESPROGRAM_HTTP_API Result<void> SetLimits(Http3QuicAdapterLimits limits) noexcept;
            LIKESPROGRAM_HTTP_API Http3QuicAdapterLimits Limits() const noexcept;
            // 截止时间由调用方拥有；adapter 不创建 timer 或线程。
            LIKESPROGRAM_HTTP_API Result<void> SetDeadline(LikesProgram::Time::Deadline deadline) noexcept;
            LIKESPROGRAM_HTTP_API Result<void> CheckDeadline() const;
            LIKESPROGRAM_HTTP_API bool HasDeadline() const noexcept;
            LIKESPROGRAM_HTTP_API bool DeadlineExpired() const noexcept;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http3QuicEvent& event);
            // Applies caller-owned send-queue feedback. DATA credit is reserved
            // when submitted and restored only for a rejected pending action;
            // rejected RESET_STREAM/STOP_SENDING actions roll back their local
            // direction flag so the caller can retry the cancellation.
            LIKESPROGRAM_HTTP_API Result<void> FeedTransportFeedback(const Http3QuicTransportFeedback& feedback);
            // A rejected DATA payload is moved to the caller exactly once.
            // Accepted DATA and control/terminal feedback return available=false.
            LIKESPROGRAM_HTTP_API Result<Http3QuicRejectedData> FeedTransportFeedbackWithRejectedData(const Http3QuicTransportFeedback& feedback);
            // Requeues one rejected pending DATA payload at the front of the
            // producer before consuming the feedback. Shared body-budget
            // ownership transfers back without a release/reserve gap.
            LIKESPROGRAM_HTTP_API Result<void> FeedTransportFeedbackAndRequeueRejectedData(const Http3QuicTransportFeedback& feedback, HttpBodyProducer& producer);
            LIKESPROGRAM_HTTP_API Result<void> SendStreamData(std::uint64_t streamId, const std::vector<std::uint8_t>& payload);
            // Submits the producer's current prepared prefix and consumes it
            // only after the local action handoff succeeds. A shared body
            // budget reservation transfers directly to the pending action.
            LIKESPROGRAM_HTTP_API Result<void> SendPreparedStreamData(std::uint64_t streamId, HttpBodyProducer& producer, std::uint64_t preparedPullId);
            // Submits strictly increasing absolute receive limits. The prior
            // committed value remains authoritative until feedback accepts.
            LIKESPROGRAM_HTTP_API Result<void> UpdateConnectionReceiveCredit(std::uint64_t newLimit);
            LIKESPROGRAM_HTTP_API Result<void> UpdateStreamReceiveCredit(std::uint64_t streamId, std::uint64_t newLimit);
            LIKESPROGRAM_HTTP_API Result<void> SendStreamFin(std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API Result<void> ResetStream(std::uint64_t streamId, std::uint64_t errorCode);
            LIKESPROGRAM_HTTP_API Result<void> StopSending(std::uint64_t streamId, std::uint64_t errorCode);
            // Apply a caller-selected, non-owning H3 failure plan after
            // validating local/peer stream direction state.
            LIKESPROGRAM_HTTP_API Result<void> ApplyRequestStreamError(std::uint64_t streamId, const Http3RequestStreamQuicActions& actions);
            // Maps body cancellation to the still-open request-stream
            // directions. PeerReset is acknowledged only after its event.
            LIKESPROGRAM_HTTP_API Result<void> ApplyBodyCancellation(std::uint64_t streamId, HttpBodyCancelReason reason);
            // Converts an observed caller-owned deadline expiry into the
            // existing feedback-confirmed connection-close handoff. The
            // caller selects the H3 application error code and owns retry.
            LIKESPROGRAM_HTTP_API Result<void> CloseExpiredDeadline(std::uint64_t errorCode);
            LIKESPROGRAM_HTTP_API Result<void> Close(std::uint64_t errorCode = 0);

            LIKESPROGRAM_HTTP_API Http3QuicAdapterState State() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QuicAdapterSnapshot Snapshot() const noexcept;
            // Returns the effective caller-owned credit for one stream. When
            // bounded is false, availableCredit is intentionally zero.
            LIKESPROGRAM_HTTP_API Result<Http3QuicSendCreditSnapshot> SendCredit(std::uint64_t streamId) const;
            LIKESPROGRAM_HTTP_API Result<Http3QuicPendingDataSnapshot> PendingData(std::uint64_t streamId) const;
            LIKESPROGRAM_HTTP_API Result<Http3QuicReceiveCreditSnapshot> ReceiveCredit(std::uint64_t streamId) const;
            LIKESPROGRAM_HTTP_API Http3QuicErrorContext LastErrorContext() const noexcept;
            LIKESPROGRAM_HTTP_API HttpErrorContext LastHttpErrorContext() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            Result<void> SendStreamDataInternal(std::uint64_t streamId, const std::vector<std::uint8_t>& payload, bool bodyBudgetTransferred);

            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
