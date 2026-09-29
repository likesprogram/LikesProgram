#include <LikesProgram/Http/Http3QuicAdapter.hpp>
#include <LikesProgram/Http/HttpBodyProducer.hpp>
#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/HttpBodyBudget.hpp>
#include <LikesProgram/Http/HttpBodyCancellation.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace LikesProgram {
    namespace Http {
        namespace {
            constexpr std::uint8_t kLocalFin = 0x01;
            constexpr std::uint8_t kPeerFin = 0x02;
            constexpr std::uint8_t kLocalReset = 0x04;
            constexpr std::uint8_t kPeerReset = 0x08;
            constexpr std::uint8_t kLocalStopSending = 0x10;
            constexpr std::uint8_t kPeerStopSending = 0x20;
            constexpr std::uint8_t kPeerObserved = 0x40;
            constexpr std::uint8_t kActionCommitted = 0x80;
            constexpr std::uint64_t kMaxQuicStreamId =
                (std::uint64_t{ 1 } << 62) - 1; // QUIC stream ID 使用 62-bit varint

            bool IsFullyClosed(std::uint8_t flags) noexcept {
                return (flags & (kLocalReset | kPeerReset)) != 0
                    || ((flags & kLocalFin) != 0 && (flags & kPeerFin) != 0);
            }

            bool IsPeerReceiveTerminal(std::uint8_t flags) noexcept {
                return (flags & (kPeerFin | kPeerReset)) != 0;
            }

            bool IsLocalSendBlocked(std::uint8_t flags) noexcept {
                return (flags & (kLocalFin | kLocalReset
                    | kPeerReset | kPeerStopSending)) != 0;
            }

            std::size_t CountActiveStreams(
                const std::unordered_map<std::uint64_t, std::uint8_t>& streams) noexcept {
                std::size_t active = 0;
                for (const auto& stream : streams) {
                    if (!IsFullyClosed(stream.second)) ++active;
                }
                return active;
            }

            std::size_t CountZeroCredits(
                const std::unordered_map<std::uint64_t, std::uint64_t>& credits) noexcept {
                std::size_t count = 0;
                for (const auto& credit : credits) {
                    if (credit.second == 0) ++count;
                }
                return count;
            }

            std::size_t CountProvisionalStreams(
                const std::unordered_map<std::uint64_t, std::uint8_t>& streams) noexcept {
                std::size_t count = 0;
                for (const auto& stream : streams) {
                    if ((stream.second & (kPeerObserved | kActionCommitted)) == 0) {
                        ++count;
                    }
                }
                return count;
            }
        }

        struct Http3QuicAdapter::Impl {
            struct PendingDataAction {
                std::uint64_t streamId = 0;
                std::size_t bytes = 0;
                std::vector<std::uint8_t> payload;
                bool bodyBudgetBound = false;
                bool streamCreditBound = false;
                std::uint64_t streamCreditVersion = 0;
                bool connectionCreditBound = false;
                std::uint64_t connectionCreditVersion = 0;
                bool provisionalStream = false;
            };

            struct PendingControlAction {
                Http3QuicActionKind kind = Http3QuicActionKind::ResetStream;
                std::uint64_t streamId = 0;
                std::uint64_t errorCode = 0;
                std::uint8_t localFlag = 0;
                std::uint8_t priorFlags = 0;
                std::uint64_t priorLastErrorCode = 0;
                bool provisionalStream = false;
            };

            struct PendingTerminalAction {
                Http3QuicActionKind kind = Http3QuicActionKind::StreamFin;
                std::uint64_t streamId = 0;
                std::uint64_t errorCode = 0;
                std::uint8_t localFlag = 0;
                std::uint8_t priorFlags = 0;
                Http3QuicAdapterState priorState =
                    Http3QuicAdapterState::Ready;
                std::uint64_t priorLastErrorCode = 0;
                bool provisionalStream = false;
            };

            struct ReceiveCreditState {
                bool set = false;
                std::uint64_t limit = 0;
                std::uint64_t pendingActionId = 0;
                std::uint64_t pendingLimit = 0;
            };

            struct PendingReceiveCreditAction {
                Http3QuicActionKind kind =
                    Http3QuicActionKind::ConnectionReceiveCredit;
                std::uint64_t streamId = 0;
                std::uint64_t limit = 0;
            };

            Http3QuicActionSink* actionSink = nullptr;
            Http3QuicEventObserver* eventObserver = nullptr;
            HttpBodyBudget* bodyBudget = nullptr;
            Http3QuicAdapterState state = Http3QuicAdapterState::AwaitingHandshake;
            Status failure;
            LikesProgram::Time::Deadline deadline =
                LikesProgram::Time::Deadline::Infinite();
            std::unordered_map<std::uint64_t, std::uint8_t> streams;
            std::size_t eventCount = 0;
            std::size_t actionCount = 0;
            std::uint64_t lastErrorCode = 0;
            Http3QuicErrorContext errorContext;
            mutable bool deadlineExpired = false;
            Http3QuicAdapterLimits limits;
            std::unordered_map<std::uint64_t, std::uint64_t> sendCredits;
            bool connectionCreditSet = false;
            std::uint64_t connectionCredit = 0;
            std::unordered_map<std::uint64_t, std::uint64_t> streamCreditVersions;
            std::uint64_t connectionCreditVersion = 0;
            bool transportBlocked = false;
            std::uint64_t nextActionId = 1;
            std::unordered_map<std::uint64_t, PendingDataAction> pendingDataActions;
            std::unordered_map<std::uint64_t, PendingControlAction> pendingControlActions;
            std::unordered_map<std::uint64_t, PendingTerminalAction> pendingTerminalActions;
            ReceiveCreditState receiveConnectionCredit;
            std::unordered_map<std::uint64_t, ReceiveCreditState> receiveStreamCredits;
            std::unordered_map<std::uint64_t, PendingReceiveCreditAction>
                pendingReceiveCreditActions;
            std::size_t pendingDataBytes = 0;
            std::size_t pendingBodyBudgetBytes = 0;

            void ReleaseBodyBudgetReservations() noexcept {
                if (bodyBudget == nullptr) return;
                for (const auto& pending : pendingDataActions) {
                    if (pending.second.bodyBudgetBound) {
                        (void)bodyBudget->Release(
                            pending.second.streamId, pending.second.bytes);
                    }
                }
            }

            void RetirePendingData(const PendingDataAction& pending) noexcept {
                pendingDataBytes -= pending.bytes;
                if (pending.bodyBudgetBound) {
                    pendingBodyBudgetBytes -= pending.bytes;
                }
            }

            ~Impl() {
                ReleaseBodyBudgetReservations();
            }

            Result<void> Fail(const Status& status,
                Http3QuicErrorContext context = {}) {
                if (failure.IsOk()) failure = status;
                if (!errorContext.valid && context.valid) errorContext = context;
                state = Http3QuicAdapterState::Failed;
                return failure;
            }

            Result<void> RequireReady() const {
                if (!failure.IsOk()) return failure;
                auto timeout = CheckDeadline();
                if (!timeout.IsOk()) return timeout;
                if (state != Http3QuicAdapterState::Ready) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC adapter is not ready");
                }
                return {};
            }

            Result<void> CheckDeadline() const {
                if (deadline.Expired()) {
                    deadlineExpired = true;
                    return Status::DeadlineExceeded(
                        u"HTTP/3 QUIC adapter deadline expired");
                }
                deadlineExpired = false;
                return {};
            }

            Result<void> RequireStreamId(std::uint64_t streamId) const {
                if (streamId > kMaxQuicStreamId) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QUIC stream id exceeds the 62-bit limit");
                }
                return {};
            }

            Result<void> RequireActionSink() const {
                if (actionSink == nullptr) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC action sink is not configured");
                }
                return {};
            }

            Result<void> SubmitClose(std::uint64_t errorCode) {
                auto result = Emit({ Http3QuicActionKind::CloseConnection,
                    0, errorCode, {} });
                if (!result.IsOk()) return result;
                // Transport rejection restores this prior Ready state through
                // the existing pending-terminal feedback transaction.
                state = Http3QuicAdapterState::Closing;
                lastErrorCode = errorCode;
                return {};
            }

            Result<void> RequireStreamCapacity(std::uint64_t streamId) const {
                if (streams.find(streamId) == streams.end()
                    && CountActiveStreams(streams) >= limits.maxActiveStreams) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC adapter active stream limit exceeded");
                }
                return {};
            }

            Result<void> Emit(Http3QuicAction action,
                bool provisionalStream = false) {
                auto sink = RequireActionSink();
                if (!sink.IsOk()) return sink;
                if (actionCount >= limits.maxActions) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC adapter action limit exceeded");
                }
                if (nextActionId == 0) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC adapter action id space exhausted");
                }
                action.actionId = nextActionId;
                if (action.kind == Http3QuicActionKind::StreamData) {
                    const auto bytes = action.payload.size();
                    const bool bodyBudgetBound =
                        bodyBudget != nullptr && bytes != 0;
                    if (bytes > std::numeric_limits<std::size_t>::max()
                        - pendingDataBytes
                        || (bodyBudgetBound
                            && bytes > std::numeric_limits<std::size_t>::max()
                                - pendingBodyBudgetBytes)) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QUIC pending DATA byte counter overflow");
                    }
                    try {
                        const auto streamCredit = sendCredits.find(action.streamId);
                        const auto streamVersion = streamCreditVersions.find(
                            action.streamId);
                        const auto inserted = pendingDataActions.emplace(
                            action.actionId,
                            PendingDataAction{
                                action.streamId,
                                bytes,
                                action.payload,
                                bodyBudgetBound,
                                streamCredit != sendCredits.end(),
                                streamVersion == streamCreditVersions.end()
                                    ? 0 : streamVersion->second,
                                connectionCreditSet,
                                connectionCreditVersion,
                                provisionalStream });
                        if (!inserted.second) {
                            return Status::Internal(
                                u"HTTP/3 QUIC adapter action id collision");
                        }
                        pendingDataBytes += bytes;
                        if (bodyBudgetBound) pendingBodyBudgetBytes += bytes;
                    }
                    catch (...) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QUIC adapter pending action limit exceeded");
                    }
                }
                else if (action.kind == Http3QuicActionKind::ResetStream
                    || action.kind == Http3QuicActionKind::StopSending) {
                    try {
                        const auto stream = streams.find(action.streamId);
                        const auto flags = stream == streams.end()
                            ? std::uint8_t{ 0 } : stream->second;
                        const auto localFlag = static_cast<std::uint8_t>(action.kind
                            == Http3QuicActionKind::ResetStream
                            ? kLocalReset : kLocalStopSending);
                        const auto inserted = pendingControlActions.emplace(
                            action.actionId,
                            PendingControlAction{
                                action.kind,
                                action.streamId,
                                action.errorCode,
                                localFlag,
                                flags,
                                lastErrorCode,
                                provisionalStream });
                        if (!inserted.second) {
                            return Status::Internal(
                                u"HTTP/3 QUIC adapter action id collision");
                        }
                    }
                    catch (...) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QUIC adapter pending action limit exceeded");
                    }
                }
                else if (action.kind == Http3QuicActionKind::StreamReceiveCredit
                    || action.kind
                        == Http3QuicActionKind::ConnectionReceiveCredit) {
                    ReceiveCreditState* creditState = nullptr;
                    if (action.kind
                        == Http3QuicActionKind::ConnectionReceiveCredit) {
                        creditState = &receiveConnectionCredit;
                    }
                    else {
                        const auto stream = receiveStreamCredits.find(
                            action.streamId);
                        if (stream == receiveStreamCredits.end()) {
                            return Status::Internal(
                                u"HTTP/3 QUIC receive-credit stream state is missing");
                        }
                        creditState = &stream->second;
                    }
                    try {
                        const auto inserted = pendingReceiveCreditActions.emplace(
                            action.actionId,
                            PendingReceiveCreditAction{
                                action.kind, action.streamId, action.flowCredit });
                        if (!inserted.second) {
                            return Status::Internal(
                                u"HTTP/3 QUIC adapter action id collision");
                        }
                    }
                    catch (...) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QUIC receive-credit pending allocation failed");
                    }
                    creditState->pendingActionId = action.actionId;
                    creditState->pendingLimit = action.flowCredit;
                }
                else if (action.kind == Http3QuicActionKind::StreamFin
                    || action.kind == Http3QuicActionKind::CloseConnection) {
                    try {
                        const auto stream = streams.find(action.streamId);
                        const auto flags = stream == streams.end()
                            ? std::uint8_t{ 0 } : stream->second;
                        const auto localFlag = action.kind
                            == Http3QuicActionKind::StreamFin
                            ? kLocalFin : std::uint8_t{ 0 };
                        const auto inserted = pendingTerminalActions.emplace(
                            action.actionId,
                            PendingTerminalAction{
                                action.kind,
                                action.streamId,
                                action.errorCode,
                                localFlag,
                                flags,
                                state,
                                lastErrorCode,
                                provisionalStream });
                        if (!inserted.second) {
                            return Status::Internal(
                                u"HTTP/3 QUIC adapter action id collision");
                        }
                    }
                    catch (...) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 QUIC adapter pending action limit exceeded");
                    }
                }
                ++actionCount;
                ++nextActionId;
                actionSink->Submit(action);
                return {};
            }

            Result<void> EmitStream(Http3QuicAction action) {
                const auto streamId = action.streamId;
                bool insertedStream = false;
                try {
                    insertedStream = streams.emplace(
                        streamId, std::uint8_t{ 0 }).second;
                }
                catch (...) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC adapter active stream ledger allocation failed");
                }

                // Local submission rolls both records back together. A newly
                // inserted slot remains owned by the pending feedback record.
                auto result = Emit(std::move(action), insertedStream);
                if (!result.IsOk() && insertedStream) {
                    streams.erase(streamId);
                }
                return result;
            }

            void MarkStreamCommitted(std::uint64_t streamId) noexcept {
                const auto stream = streams.find(streamId);
                if (stream != streams.end()) {
                    stream->second = static_cast<std::uint8_t>(
                        stream->second | kActionCommitted);
                }
            }

            void ResolveRejectedProvisionalStream(
                std::uint64_t actionId,
                std::uint64_t streamId,
                bool provisionalStream) noexcept {
                if (!provisionalStream) return;
                const auto stream = streams.find(streamId);
                if (stream == streams.end()
                    || (stream->second & (kPeerObserved | kActionCommitted)) != 0) {
                    return;
                }

                std::uint64_t transferActionId =
                    std::numeric_limits<std::uint64_t>::max();
                PendingDataAction* nextData = nullptr;
                PendingControlAction* nextControl = nullptr;
                PendingTerminalAction* nextTerminal = nullptr;
                for (auto& pending : pendingDataActions) {
                    if (pending.first != actionId
                        && pending.second.streamId == streamId
                        && pending.first < transferActionId) {
                        transferActionId = pending.first;
                        nextData = &pending.second;
                        nextControl = nullptr;
                        nextTerminal = nullptr;
                    }
                }
                for (auto& pending : pendingControlActions) {
                    if (pending.first != actionId
                        && pending.second.streamId == streamId
                        && pending.first < transferActionId) {
                        transferActionId = pending.first;
                        nextData = nullptr;
                        nextControl = &pending.second;
                        nextTerminal = nullptr;
                    }
                }
                for (auto& pending : pendingTerminalActions) {
                    if (pending.first != actionId
                        && pending.second.kind == Http3QuicActionKind::StreamFin
                        && pending.second.streamId == streamId
                        && pending.first < transferActionId) {
                        transferActionId = pending.first;
                        nextData = nullptr;
                        nextControl = nullptr;
                        nextTerminal = &pending.second;
                    }
                }
                if (nextData != nullptr) {
                    nextData->provisionalStream = true;
                    return;
                }
                if (nextControl != nullptr) {
                    nextControl->provisionalStream = true;
                    return;
                }
                if (nextTerminal != nullptr) {
                    nextTerminal->provisionalStream = true;
                    return;
                }

                streams.erase(stream);
            }

            void RestoreCredit(std::uint64_t& credit, std::size_t bytes) noexcept {
                const auto amount = static_cast<std::uint64_t>(bytes);
                if (credit > std::numeric_limits<std::uint64_t>::max() - amount) {
                    credit = std::numeric_limits<std::uint64_t>::max();
                }
                else {
                    credit += amount;
                }
            }

            Result<void> StreamEvent(const Http3QuicEvent& event) {
                auto ready = RequireReady();
                if (!ready.IsOk()) return ready;
                const Http3QuicErrorContext context{ true, event.kind,
                    event.streamId, event.errorCode };
                auto stream = RequireStreamId(event.streamId);
                if (!stream.IsOk()) return Fail(stream.GetStatus(), context);
                auto capacity = RequireStreamCapacity(event.streamId);
                if (!capacity.IsOk()) return capacity;
                if (event.kind == Http3QuicEventKind::StreamData
                    && event.payload.size() > limits.maxPayloadBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC adapter event payload limit exceeded");
                }

                auto& flags = streams[event.streamId];
                switch (event.kind) {
                case Http3QuicEventKind::StreamData:
                    if (IsPeerReceiveTerminal(flags)
                        || (flags & kLocalStopSending) != 0) {
                        return Fail(Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QUIC stream is already terminal"), context);
                    }
                    break;
                case Http3QuicEventKind::StreamFin:
                    if (IsPeerReceiveTerminal(flags)) {
                        return Fail(Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QUIC stream FIN is duplicated"), context);
                    }
                    flags = static_cast<std::uint8_t>(flags | kPeerFin);
                    break;
                case Http3QuicEventKind::StreamReset:
                    if ((flags & kPeerReset) != 0) {
                        return Fail(Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QUIC stream reset is duplicated"), context);
                    }
                    flags = static_cast<std::uint8_t>(flags | kPeerReset);
                    lastErrorCode = event.errorCode;
                    break;
                case Http3QuicEventKind::StopSending:
                    if ((flags & kPeerStopSending) != 0) {
                        return Fail(Status(StatusCode::FailedPrecondition,
                            u"HTTP/3 QUIC STOP_SENDING is duplicated"), context);
                    }
                    flags = static_cast<std::uint8_t>(flags | kPeerStopSending);
                    lastErrorCode = event.errorCode;
                    break;
                case Http3QuicEventKind::StreamSendCredit:
                    if (!event.payload.empty()) {
                        return Fail(Status::InvalidArgument(
                            u"HTTP/3 QUIC stream credit event cannot carry payload"), context);
                    }
                    sendCredits[event.streamId] = event.flowCredit;
                    ++streamCreditVersions[event.streamId];
                    break;
                default:
                    return Fail(Status::InvalidArgument(
                        u"invalid HTTP/3 QUIC stream event"), context);
                }
                flags = static_cast<std::uint8_t>(flags | kPeerObserved);
                return {};
            }
        };

        Http3QuicAdapter::Http3QuicAdapter(
            Http3QuicActionSink* actionSink,
            Http3QuicEventObserver* eventObserver)
            : m_impl(std::make_unique<Impl>()) {
            m_impl->actionSink = actionSink;
            m_impl->eventObserver = eventObserver;
        }

        Http3QuicAdapter::~Http3QuicAdapter() = default;
        Http3QuicAdapter::Http3QuicAdapter(Http3QuicAdapter&&) noexcept = default;
        Http3QuicAdapter& Http3QuicAdapter::operator=(Http3QuicAdapter&&) noexcept = default;

        Result<void> Http3QuicAdapter::AttachActionSink(
            Http3QuicActionSink* sink) noexcept {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (sink == nullptr) return Status::InvalidArgument(
                u"HTTP/3 QUIC action sink must not be null");
            if (m_impl->actionSink != nullptr && m_impl->actionSink != sink) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC action sink is already attached");
            }
            m_impl->actionSink = sink;
            return {};
        }

        Result<void> Http3QuicAdapter::AttachEventObserver(
            Http3QuicEventObserver* observer) noexcept {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (observer == nullptr) return Status::InvalidArgument(
                u"HTTP/3 QUIC event observer must not be null");
            if (m_impl->eventObserver != nullptr && m_impl->eventObserver != observer) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC event observer is already attached");
            }
            m_impl->eventObserver = observer;
            return {};
        }

        Result<void> Http3QuicAdapter::AttachBodyBudget(
            HttpBodyBudget* budget) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (budget == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC adapter body budget must not be null");
            }
            if (m_impl->bodyBudget != nullptr) {
                if (m_impl->bodyBudget == budget) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC adapter already has a body budget");
            }
            if (!m_impl->pendingDataActions.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC body budget must attach before pending DATA");
            }
            const auto budgetStatus = budget->LastError();
            if (!budgetStatus.IsOk()) return budgetStatus;
            m_impl->bodyBudget = budget;
            return {};
        }

        bool Http3QuicAdapter::HasBodyBudget() const noexcept {
            return m_impl != nullptr && m_impl->bodyBudget != nullptr;
        }

        Result<void> Http3QuicAdapter::SetLimits(
            Http3QuicAdapterLimits limits) noexcept {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (limits.maxActiveStreams == 0
                || limits.maxEvents == 0
                || limits.maxActions == 0
                || limits.maxPayloadBytes == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC adapter limits must be non-zero");
            }
            if (CountActiveStreams(m_impl->streams) > limits.maxActiveStreams
                || m_impl->eventCount > limits.maxEvents
                || m_impl->actionCount > limits.maxActions) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC adapter limits are below current usage");
            }
            m_impl->limits = limits;
            return {};
        }

        Http3QuicAdapterLimits Http3QuicAdapter::Limits() const noexcept {
            return m_impl ? m_impl->limits : Http3QuicAdapterLimits{};
        }

        Result<void> Http3QuicAdapter::SetDeadline(
            LikesProgram::Time::Deadline deadline) noexcept {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            m_impl->deadline = deadline;
            return m_impl->CheckDeadline();
        }

        Result<void> Http3QuicAdapter::CheckDeadline() const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            return m_impl->CheckDeadline();
        }

        bool Http3QuicAdapter::HasDeadline() const noexcept {
            return m_impl != nullptr && m_impl->deadline.HasDeadline();
        }

        bool Http3QuicAdapter::DeadlineExpired() const noexcept {
            return m_impl == nullptr || m_impl->deadline.Expired();
        }

        Result<void> Http3QuicAdapter::Feed(const Http3QuicEvent& event) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->eventCount >= m_impl->limits.maxEvents) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC adapter event limit exceeded");
            }

            if (event.kind == Http3QuicEventKind::HandshakeComplete) {
                if (m_impl->state != Http3QuicAdapterState::AwaitingHandshake) {
                    return m_impl->Fail(Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC handshake event is duplicated or late"),
                        { true, event.kind, event.streamId, event.errorCode });
                }
                m_impl->state = Http3QuicAdapterState::Ready;
            } else if (event.kind == Http3QuicEventKind::ConnectionClose) {
                if (m_impl->state != Http3QuicAdapterState::Ready
                    && m_impl->state != Http3QuicAdapterState::Closing) {
                    return m_impl->Fail(Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC connection close event is out of order"),
                        { true, event.kind, event.streamId, event.errorCode });
                }
                if (event.payload.size() > m_impl->limits.maxPayloadBytes
                    || event.payload.size() > kHttp3QuicMaxCloseReasonBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC connection close reason exceeds the adapter limit");
                }
                m_impl->state = Http3QuicAdapterState::Closed;
                m_impl->lastErrorCode = event.errorCode;
                m_impl->errorContext = {
                    true,
                    event.kind,
                    event.streamId,
                    event.errorCode,
                    false,
                    event.applicationError
                };
                m_impl->errorContext.closeReasonSize = event.payload.size();
                std::copy(event.payload.begin(), event.payload.end(),
                    m_impl->errorContext.closeReason.begin());
            } else if (event.kind == Http3QuicEventKind::ConnectionSendCredit) {
                if (event.streamId != 0 || !event.payload.empty()) {
                    return m_impl->Fail(Status::InvalidArgument(
                        u"HTTP/3 QUIC connection credit event coordinates are invalid"),
                        { true, event.kind, event.streamId, event.errorCode });
                }
                m_impl->connectionCreditSet = true;
                m_impl->connectionCredit = event.flowCredit;
                ++m_impl->connectionCreditVersion;
            } else {
                auto result = m_impl->StreamEvent(event);
                if (!result.IsOk()) return result;
            }

            ++m_impl->eventCount;
            if (m_impl->eventObserver != nullptr) {
                m_impl->eventObserver->Observe(event);
            }
            return {};
        }

        Result<void> Http3QuicAdapter::FeedTransportFeedback(
            const Http3QuicTransportFeedback& feedback) {
            auto result = FeedTransportFeedbackWithRejectedData(feedback);
            if (!result.IsOk()) return result.GetStatus();
            return {};
        }

        Result<Http3QuicRejectedData>
            Http3QuicAdapter::FeedTransportFeedbackWithRejectedData(
                const Http3QuicTransportFeedback& feedback) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;

            switch (feedback.kind) {
            case Http3QuicTransportFeedbackKind::Writable:
            case Http3QuicTransportFeedbackKind::Blocked:
                if (feedback.actionId != 0
                    || feedback.acceptedBytes != 0
                    || feedback.errorCode != 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QUIC transport state feedback has invalid coordinates");
                }
                m_impl->transportBlocked =
                    feedback.kind == Http3QuicTransportFeedbackKind::Blocked;
                return Http3QuicRejectedData{};

            case Http3QuicTransportFeedbackKind::ActionAccepted:
            case Http3QuicTransportFeedbackKind::ActionRejected: {
                if (feedback.actionId == 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QUIC transport action feedback requires an action id");
                }
                const auto pendingData =
                    m_impl->pendingDataActions.find(feedback.actionId);
                const auto pendingControl =
                    m_impl->pendingControlActions.find(feedback.actionId);
                const auto pendingTerminal =
                    m_impl->pendingTerminalActions.find(feedback.actionId);
                const auto pendingReceiveCredit =
                    m_impl->pendingReceiveCreditActions.find(feedback.actionId);
                if (pendingData == m_impl->pendingDataActions.end()
                    && pendingControl == m_impl->pendingControlActions.end()
                    && pendingTerminal == m_impl->pendingTerminalActions.end()
                    && pendingReceiveCredit
                        == m_impl->pendingReceiveCreditActions.end()) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC transport action feedback is unknown or duplicated");
                }
                if (pendingReceiveCredit
                    != m_impl->pendingReceiveCreditActions.end()
                    && (feedback.acceptedBytes != 0
                        || (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted
                            && feedback.errorCode != 0))) {
                    return Status(StatusCode::InvalidArgument,
                        u"HTTP/3 QUIC receive-credit feedback has invalid coordinates");
                }
                if (feedback.kind == Http3QuicTransportFeedbackKind::ActionRejected) {
                    m_impl->errorContext.transportFeedbackValid = true;
                    m_impl->errorContext.transportFeedbackKind = feedback.kind;
                    m_impl->errorContext.transportActionId = feedback.actionId;
                    m_impl->errorContext.transportErrorCode = feedback.errorCode;
                }
                if (pendingReceiveCredit
                    != m_impl->pendingReceiveCreditActions.end()) {
                    const auto pending = pendingReceiveCredit->second;
                    if (pending.kind
                        == Http3QuicActionKind::ConnectionReceiveCredit) {
                        auto& credit = m_impl->receiveConnectionCredit;
                        if (credit.pendingActionId != feedback.actionId
                            || credit.pendingLimit != pending.limit) {
                            return Status::Internal(
                                u"HTTP/3 QUIC connection receive-credit ledger diverged");
                        }
                        if (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted) {
                            credit.set = true;
                            credit.limit = pending.limit;
                        }
                        credit.pendingActionId = 0;
                        credit.pendingLimit = 0;
                    }
                    else {
                        const auto stream = m_impl->receiveStreamCredits.find(
                            pending.streamId);
                        if (stream == m_impl->receiveStreamCredits.end()
                            || stream->second.pendingActionId != feedback.actionId
                            || stream->second.pendingLimit != pending.limit) {
                            return Status::Internal(
                                u"HTTP/3 QUIC stream receive-credit ledger diverged");
                        }
                        if (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted) {
                            stream->second.set = true;
                            stream->second.limit = pending.limit;
                        }
                        stream->second.pendingActionId = 0;
                        stream->second.pendingLimit = 0;
                        if (!stream->second.set) {
                            m_impl->receiveStreamCredits.erase(stream);
                        }
                    }
                    m_impl->pendingReceiveCreditActions.erase(
                        pendingReceiveCredit);
                    return Http3QuicRejectedData{};
                }
                if (pendingControl != m_impl->pendingControlActions.end()) {
                    if (feedback.acceptedBytes != 0
                        || (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted
                            && feedback.errorCode != 0)) {
                        return Status(StatusCode::InvalidArgument,
                            u"HTTP/3 QUIC control action feedback has invalid coordinates");
                    }
                    if (feedback.kind
                        == Http3QuicTransportFeedbackKind::ActionRejected) {
                        const auto stream = m_impl->streams.find(
                            pendingControl->second.streamId);
                        if (stream != m_impl->streams.end()) {
                            stream->second = static_cast<std::uint8_t>(
                                (stream->second & ~pendingControl->second.localFlag)
                                | (pendingControl->second.priorFlags
                                    & pendingControl->second.localFlag));
                        }
                        if (m_impl->lastErrorCode
                            == pendingControl->second.errorCode) {
                            m_impl->lastErrorCode =
                                pendingControl->second.priorLastErrorCode;
                        }
                    }
                    if (feedback.kind
                        == Http3QuicTransportFeedbackKind::ActionAccepted) {
                        m_impl->MarkStreamCommitted(
                            pendingControl->second.streamId);
                    }
                    else {
                        m_impl->ResolveRejectedProvisionalStream(
                            feedback.actionId,
                            pendingControl->second.streamId,
                            pendingControl->second.provisionalStream);
                    }
                    m_impl->pendingControlActions.erase(pendingControl);
                    return Http3QuicRejectedData{};
                }
                if (pendingTerminal != m_impl->pendingTerminalActions.end()) {
                    if (feedback.acceptedBytes != 0
                        || (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted
                            && feedback.errorCode != 0)) {
                        return Status(StatusCode::InvalidArgument,
                            u"HTTP/3 QUIC terminal action feedback has invalid coordinates");
                    }
                    if (feedback.kind
                        == Http3QuicTransportFeedbackKind::ActionRejected) {
                        if (pendingTerminal->second.kind
                            == Http3QuicActionKind::StreamFin) {
                            const auto stream = m_impl->streams.find(
                                pendingTerminal->second.streamId);
                            if (stream != m_impl->streams.end()) {
                                stream->second = static_cast<std::uint8_t>(
                                    (stream->second
                                        & ~pendingTerminal->second.localFlag)
                                    | (pendingTerminal->second.priorFlags
                                        & pendingTerminal->second.localFlag));
                            }
                        }
                        else {
                            m_impl->state = pendingTerminal->second.priorState;
                        }
                        if (m_impl->lastErrorCode
                            == pendingTerminal->second.errorCode) {
                            m_impl->lastErrorCode =
                                pendingTerminal->second.priorLastErrorCode;
                        }
                    }
                    if (pendingTerminal->second.kind
                        == Http3QuicActionKind::StreamFin) {
                        if (feedback.kind
                            == Http3QuicTransportFeedbackKind::ActionAccepted) {
                            m_impl->MarkStreamCommitted(
                                pendingTerminal->second.streamId);
                        }
                        else {
                            m_impl->ResolveRejectedProvisionalStream(
                                feedback.actionId,
                                pendingTerminal->second.streamId,
                                pendingTerminal->second.provisionalStream);
                        }
                    }
                    m_impl->pendingTerminalActions.erase(pendingTerminal);
                    return Http3QuicRejectedData{};
                }
                const auto bytes = static_cast<std::uint64_t>(
                    pendingData->second.bytes);
                if (feedback.kind == Http3QuicTransportFeedbackKind::ActionAccepted) {
                    if (feedback.acceptedBytes != bytes || feedback.errorCode != 0) {
                        return Status::InvalidArgument(
                            u"HTTP/3 QUIC accepted DATA feedback has an invalid byte count");
                    }
                }
                else if (feedback.acceptedBytes != 0) {
                    return Status::InvalidArgument(
                        u"HTTP/3 QUIC rejected DATA feedback must not accept bytes");
                }

                if (pendingData->second.bodyBudgetBound) {
                    if (m_impl->bodyBudget == nullptr) {
                        return Status::Internal(
                            u"HTTP/3 QUIC pending DATA lost its body budget");
                    }
                    auto released = m_impl->bodyBudget->Release(
                        pendingData->second.streamId,
                        pendingData->second.bytes);
                    if (!released.IsOk()) return released.GetStatus();
                }

                if (feedback.kind == Http3QuicTransportFeedbackKind::ActionRejected) {
                    const auto streamCredit = m_impl->sendCredits.find(
                        pendingData->second.streamId);
                    const auto streamVersion = m_impl->streamCreditVersions.find(
                        pendingData->second.streamId);
                    if (pendingData->second.streamCreditBound
                        && streamCredit != m_impl->sendCredits.end()
                        && streamVersion != m_impl->streamCreditVersions.end()
                        && streamVersion->second == pendingData->second.streamCreditVersion) {
                        m_impl->RestoreCredit(streamCredit->second, pendingData->second.bytes);
                    }
                    if (pendingData->second.connectionCreditBound
                        && m_impl->connectionCreditSet
                        && m_impl->connectionCreditVersion
                            == pendingData->second.connectionCreditVersion) {
                        m_impl->RestoreCredit(m_impl->connectionCredit,
                            pendingData->second.bytes);
                    }
                }
                if (feedback.kind
                    == Http3QuicTransportFeedbackKind::ActionAccepted) {
                    m_impl->MarkStreamCommitted(pendingData->second.streamId);
                }
                else {
                    m_impl->ResolveRejectedProvisionalStream(
                        feedback.actionId,
                        pendingData->second.streamId,
                        pendingData->second.provisionalStream);
                }
                Http3QuicRejectedData rejected;
                if (feedback.kind == Http3QuicTransportFeedbackKind::ActionRejected) {
                    rejected.available = true;
                    rejected.actionId = feedback.actionId;
                    rejected.streamId = pendingData->second.streamId;
                    rejected.payload = std::move(pendingData->second.payload);
                }
                Result<Http3QuicRejectedData> result(std::move(rejected));
                m_impl->RetirePendingData(pendingData->second);
                m_impl->pendingDataActions.erase(pendingData);
                return result;
            }
            default:
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC transport feedback kind is not supported");
            }
        }

        Result<void>
            Http3QuicAdapter::FeedTransportFeedbackAndRequeueRejectedData(
                const Http3QuicTransportFeedback& feedback,
                HttpBodyProducer& producer) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (feedback.kind
                != Http3QuicTransportFeedbackKind::ActionRejected) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC producer requeue requires rejected DATA feedback");
            }
            if (feedback.actionId == 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC transport action feedback requires an action id");
            }
            if (feedback.acceptedBytes != 0) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC rejected DATA feedback must not accept bytes");
            }

            const auto pendingData =
                m_impl->pendingDataActions.find(feedback.actionId);
            if (pendingData == m_impl->pendingDataActions.end()) {
                if (m_impl->pendingControlActions.find(feedback.actionId)
                        != m_impl->pendingControlActions.end()
                    || m_impl->pendingTerminalActions.find(feedback.actionId)
                        != m_impl->pendingTerminalActions.end()
                    || m_impl->pendingReceiveCreditActions.find(feedback.actionId)
                        != m_impl->pendingReceiveCreditActions.end()) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 QUIC producer requeue requires a pending DATA action");
                }
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC transport action feedback is unknown or duplicated");
            }

            const auto streamId = pendingData->second.streamId;
            const auto bytes = pendingData->second.bytes;
            const auto bodyBudgetBound = pendingData->second.bodyBudgetBound;
            if (bodyBudgetBound) {
                if (m_impl->bodyBudget == nullptr) {
                    return Status::Internal(
                        u"HTTP/3 QUIC pending DATA lost its body budget");
                }
                const auto budgetError = m_impl->bodyBudget->LastError();
                if (!budgetError.IsOk()) return budgetError;
                if (m_impl->bodyBudget->ReservedBytes(streamId) < bytes) {
                    return Status::Internal(
                        u"HTTP/3 QUIC pending DATA lost its body budget reservation");
                }
            }

            auto transferBudget =
                producer.ValidateRejectedDataRequeueForAdapter(
                    bytes, m_impl->bodyBudget, streamId, bodyBudgetBound,
                    feedback.actionId);
            if (!transferBudget.IsOk()) return transferBudget.GetStatus();
            auto prepended = producer.PrependRejectedDataForAdapter(
                pendingData->second.payload, streamId, feedback.actionId);
            if (!prepended.IsOk()) return prepended;

            const auto streamCredit = m_impl->sendCredits.find(streamId);
            const auto streamVersion =
                m_impl->streamCreditVersions.find(streamId);
            if (pendingData->second.streamCreditBound
                && streamCredit != m_impl->sendCredits.end()
                && streamVersion != m_impl->streamCreditVersions.end()
                && streamVersion->second
                    == pendingData->second.streamCreditVersion) {
                m_impl->RestoreCredit(streamCredit->second, bytes);
            }
            if (pendingData->second.connectionCreditBound
                && m_impl->connectionCreditSet
                && m_impl->connectionCreditVersion
                    == pendingData->second.connectionCreditVersion) {
                m_impl->RestoreCredit(m_impl->connectionCredit, bytes);
            }

            m_impl->errorContext.transportFeedbackValid = true;
            m_impl->errorContext.transportFeedbackKind = feedback.kind;
            m_impl->errorContext.transportActionId = feedback.actionId;
            m_impl->errorContext.transportErrorCode = feedback.errorCode;
            m_impl->ResolveRejectedProvisionalStream(
                feedback.actionId,
                streamId,
                pendingData->second.provisionalStream);
            m_impl->RetirePendingData(pendingData->second);
            m_impl->pendingDataActions.erase(pendingData);

            Result<void> released;
            if (bodyBudgetBound && !transferBudget.Value()) {
                released = m_impl->bodyBudget->Release(streamId, bytes);
            }
            producer.FinishRejectedDataRequeueForAdapter();
            return released;
        }

        Result<void> Http3QuicAdapter::SendStreamData(
            std::uint64_t streamId,
            const std::vector<std::uint8_t>& payload) {
            return SendStreamDataInternal(streamId, payload, false);
        }

        Result<void> Http3QuicAdapter::SendPreparedStreamData(
            std::uint64_t streamId,
            HttpBodyProducer& producer,
            std::uint64_t preparedPullId) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto transferBudget = producer.ValidatePreparedPullForAdapter(
                preparedPullId, m_impl->bodyBudget, streamId);
            if (!transferBudget.IsOk()) return transferBudget.GetStatus();
            auto payload = producer.CopyPreparedPullForAdapter(preparedPullId);
            if (!payload.IsOk()) return payload.GetStatus();
            auto submitted = SendStreamDataInternal(
                streamId, payload.Value(), transferBudget.Value());
            if (!submitted.IsOk()) return submitted;
            producer.CommitPreparedPullForAdapter(transferBudget.Value());
            return {};
        }

        Result<void> Http3QuicAdapter::UpdateConnectionReceiveCredit(
            std::uint64_t newLimit) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            if (m_impl->transportBlocked) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC transport send queue is blocked");
            }
            if (newLimit == 0 || newLimit > kMaxQuicStreamId) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC connection receive credit is outside 62-bit range");
            }
            auto& credit = m_impl->receiveConnectionCredit;
            if (credit.pendingActionId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC connection receive-credit update is pending");
            }
            if (credit.set && newLimit <= credit.limit) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC connection receive credit must increase");
            }
            Http3QuicAction action;
            action.kind = Http3QuicActionKind::ConnectionReceiveCredit;
            action.flowCredit = newLimit;
            return m_impl->Emit(std::move(action));
        }

        Result<void> Http3QuicAdapter::UpdateStreamReceiveCredit(
            std::uint64_t streamId,
            std::uint64_t newLimit) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            if (m_impl->transportBlocked) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC transport send queue is blocked");
            }
            auto validStream = m_impl->RequireStreamId(streamId);
            if (!validStream.IsOk()) return validStream;
            if (newLimit == 0 || newLimit > kMaxQuicStreamId) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC stream receive credit is outside 62-bit range");
            }
            const auto stream = m_impl->streams.find(streamId);
            if (stream == m_impl->streams.end()
                || IsPeerReceiveTerminal(stream->second)) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC stream cannot receive a credit update");
            }

            bool inserted = false;
            std::unordered_map<std::uint64_t, Impl::ReceiveCreditState>::iterator credit;
            try {
                auto entry = m_impl->receiveStreamCredits.emplace(
                    streamId, Impl::ReceiveCreditState{});
                credit = entry.first;
                inserted = entry.second;
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC stream receive-credit allocation failed");
            }
            if (credit->second.pendingActionId != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC stream receive-credit update is pending");
            }
            if (credit->second.set && newLimit <= credit->second.limit) {
                return Status::InvalidArgument(
                    u"HTTP/3 QUIC stream receive credit must increase");
            }
            Http3QuicAction action;
            action.kind = Http3QuicActionKind::StreamReceiveCredit;
            action.streamId = streamId;
            action.flowCredit = newLimit;
            auto emitted = m_impl->Emit(std::move(action));
            if (!emitted.IsOk() && inserted) {
                m_impl->receiveStreamCredits.erase(credit);
            }
            return emitted;
        }

        Result<void> Http3QuicAdapter::SendStreamDataInternal(
            std::uint64_t streamId,
            const std::vector<std::uint8_t>& payload,
            bool bodyBudgetTransferred) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (bodyBudgetTransferred
                && (m_impl->bodyBudget == nullptr || payload.empty())) {
                return Status::Internal(
                    u"HTTP/3 QUIC transferred body budget is invalid");
            }
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            if (m_impl->transportBlocked) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC transport send queue is blocked");
            }
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            if (payload.size() > m_impl->limits.maxPayloadBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC adapter action payload limit exceeded");
            }
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;
            const auto streamState = m_impl->streams.find(streamId);
            const auto flags = streamState == m_impl->streams.end()
                ? std::uint8_t{ 0 } : streamState->second;
            if (IsLocalSendBlocked(flags)) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QUIC stream is already terminal");
            const auto credit = m_impl->sendCredits.find(streamId);
            if (credit != m_impl->sendCredits.end()) {
                if (payload.size() > credit->second) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC stream send credit is exhausted");
                }
            }
            if (m_impl->connectionCreditSet
                && payload.size() > m_impl->connectionCredit) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC connection send credit is exhausted");
            }
            Http3QuicAction action;
            try {
                action = { Http3QuicActionKind::StreamData,
                    streamId, 0, payload };
            }
            catch (...) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 QUIC DATA action payload allocation failed");
            }

            bool budgetReserved = false;
            if (!bodyBudgetTransferred
                && m_impl->bodyBudget != nullptr
                && !payload.empty()) {
                auto reserved = m_impl->bodyBudget->Reserve(
                    streamId, payload.size());
                if (!reserved.IsOk()) return reserved.GetStatus();
                if (reserved.Value() != payload.size()) {
                    if (reserved.Value() != 0) {
                        auto released = m_impl->bodyBudget->Release(
                            streamId, reserved.Value());
                        if (!released.IsOk()) return released.GetStatus();
                    }
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 QUIC pending DATA body budget exhausted");
                }
                budgetReserved = true;
            }

            auto result = m_impl->EmitStream(std::move(action));
            if (!result.IsOk()) {
                if (budgetReserved) {
                    auto released = m_impl->bodyBudget->Release(
                        streamId, payload.size());
                    if (!released.IsOk()) return released.GetStatus();
                }
                return result;
            }
            if (credit != m_impl->sendCredits.end()) {
                credit->second -= payload.size();
            }
            if (m_impl->connectionCreditSet) {
                m_impl->connectionCredit -= payload.size();
            }
            return {};
        }

        Result<void> Http3QuicAdapter::SendStreamFin(std::uint64_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;
            const auto streamState = m_impl->streams.find(streamId);
            const auto flags = streamState == m_impl->streams.end()
                ? std::uint8_t{ 0 } : streamState->second;
            if (IsLocalSendBlocked(flags)) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 QUIC stream FIN is duplicated");
            auto result = m_impl->EmitStream({ Http3QuicActionKind::StreamFin,
                streamId, 0, {} });
            if (!result.IsOk()) return result;
            m_impl->streams.find(streamId)->second = static_cast<std::uint8_t>(
                flags | kLocalFin);
            return {};
        }

        Result<void> Http3QuicAdapter::ResetStream(
            std::uint64_t streamId,
            std::uint64_t errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;
            const auto streamState = m_impl->streams.find(streamId);
            const auto flags = streamState == m_impl->streams.end()
                ? std::uint8_t{ 0 } : streamState->second;
            if ((flags & (kLocalReset | kLocalFin | kPeerReset)) != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC stream reset is duplicated");
            }
            auto result = m_impl->EmitStream({ Http3QuicActionKind::ResetStream,
                streamId, errorCode, {} });
            if (!result.IsOk()) return result;
            m_impl->streams.find(streamId)->second = static_cast<std::uint8_t>(
                flags | kLocalReset);
            m_impl->lastErrorCode = errorCode;
            return {};
        }

        Result<void> Http3QuicAdapter::StopSending(
            std::uint64_t streamId,
            std::uint64_t errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;
            const auto streamState = m_impl->streams.find(streamId);
            const auto flags = streamState == m_impl->streams.end()
                ? std::uint8_t{ 0 } : streamState->second;
            if ((flags & (kLocalStopSending | kPeerFin | kPeerReset)) != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC STOP_SENDING is duplicated");
            }
            auto result = m_impl->EmitStream({ Http3QuicActionKind::StopSending,
                streamId, errorCode, {} });
            if (!result.IsOk()) return result;
            m_impl->streams.find(streamId)->second = static_cast<std::uint8_t>(
                flags | kLocalStopSending);
            m_impl->lastErrorCode = errorCode;
            return {};
        }

        Result<void> Http3QuicAdapter::ApplyRequestStreamError(
            std::uint64_t streamId,
            const Http3RequestStreamQuicActions& actions) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            if (actions.quicErrorCode == 0
                || actions.quicErrorCode > kMaxQuicStreamId
                || (!actions.resetStream && !actions.stopSending)) {
                return Status::InvalidArgument(
                    u"HTTP/3 request stream action plan is empty or out of range");
            }
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;
            const auto streamState = m_impl->streams.find(streamId);
            const auto flags = streamState == m_impl->streams.end()
                ? std::uint8_t{ 0 } : streamState->second;
            if (actions.resetStream
                && (flags & (kLocalReset | kLocalFin | kPeerReset)) != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream reset direction is already terminal");
            }
            if (actions.stopSending
                && (flags & (kLocalStopSending | kPeerFin | kPeerReset)) != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream stop direction is already terminal");
            }
            auto sink = m_impl->RequireActionSink();
            if (!sink.IsOk()) return sink;
            const std::size_t requiredActions =
                (actions.resetStream ? 1U : 0U) + (actions.stopSending ? 1U : 0U);
            if (requiredActions > m_impl->limits.maxActions - m_impl->actionCount) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 request stream action plan exceeds the action limit");
            }

            if (actions.resetStream) {
                auto result = m_impl->EmitStream({ Http3QuicActionKind::ResetStream,
                    streamId, actions.quicErrorCode, {} });
                if (!result.IsOk()) return result;
            }
            if (actions.stopSending) {
                auto result = m_impl->EmitStream({ Http3QuicActionKind::StopSending,
                    streamId, actions.quicErrorCode, {} });
                if (!result.IsOk()) return result;
            }
            auto& committedFlags = m_impl->streams.find(streamId)->second;
            if (actions.resetStream) committedFlags = static_cast<std::uint8_t>(
                committedFlags | kLocalReset);
            if (actions.stopSending) committedFlags = static_cast<std::uint8_t>(
                committedFlags | kLocalStopSending);
            m_impl->lastErrorCode = actions.quicErrorCode;
            return {};
        }

        Result<void> Http3QuicAdapter::ApplyBodyCancellation(
            std::uint64_t streamId,
            HttpBodyCancelReason reason) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream;
            auto capacity = m_impl->RequireStreamCapacity(streamId);
            if (!capacity.IsOk()) return capacity;

            const auto entry = m_impl->streams.find(streamId);
            const std::uint8_t flags = entry == m_impl->streams.end() ? 0 : entry->second;
            if (reason == HttpBodyCancelReason::PeerReset) {
                if ((flags & kPeerReset) == 0) {
                    return Status(StatusCode::FailedPrecondition,
                        u"HTTP/3 peer-reset cancellation requires a peer reset event");
                }
                return {};
            }

            std::uint64_t errorCode = 0;
            switch (reason) {
            case HttpBodyCancelReason::Application:
            case HttpBodyCancelReason::DeadlineExceeded:
                errorCode = static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled);
                break;
            case HttpBodyCancelReason::ProtocolError:
                errorCode = static_cast<std::uint64_t>(Http3ErrorCode::MessageError);
                break;
            case HttpBodyCancelReason::PeerReset:
                break;
            default:
                return Status::InvalidArgument(
                    u"HTTP/3 body cancellation reason is not supported");
            }

            const bool resetStream =
                (flags & (kLocalReset | kLocalFin | kPeerReset)) == 0;
            const bool stopSending =
                (flags & (kLocalStopSending | kPeerFin | kPeerReset)) == 0;
            if (!resetStream && !stopSending) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 body cancellation directions are already terminal");
            }
            return ApplyRequestStreamError(streamId,
                Http3RequestStreamQuicActions{
                    errorCode, resetStream, stopSending });
        }

        Result<void> Http3QuicAdapter::Close(std::uint64_t errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto ready = m_impl->RequireReady();
            if (!ready.IsOk()) return ready;
            return m_impl->SubmitClose(errorCode);
        }

        Result<void> Http3QuicAdapter::CloseExpiredDeadline(
            std::uint64_t errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            if (!m_impl->failure.IsOk()) return m_impl->failure;
            if (m_impl->state != Http3QuicAdapterState::Ready) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC adapter is not ready for a deadline close");
            }

            // A successful check means the caller has no expired deadline to
            // convert; this path intentionally emits no transport action.
            auto timeout = m_impl->CheckDeadline();
            if (timeout.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 QUIC adapter deadline has not expired");
            }
            if (timeout.GetStatus().Code() != StatusCode::DeadlineExceeded) {
                return timeout;
            }
            return m_impl->SubmitClose(errorCode);
        }

        Http3QuicAdapterState Http3QuicAdapter::State() const noexcept {
            return m_impl ? m_impl->state : Http3QuicAdapterState::Failed;
        }

        Http3QuicAdapterSnapshot Http3QuicAdapter::Snapshot() const noexcept {
            if (!m_impl) return { Http3QuicAdapterState::Failed, false, 0, 0, 0, 0,
                false, 0, 0, 0, false, 0 };
            return { m_impl->state,
                m_impl->state != Http3QuicAdapterState::AwaitingHandshake
                    && m_impl->state != Http3QuicAdapterState::Failed,
                CountActiveStreams(m_impl->streams), m_impl->eventCount,
                m_impl->actionCount, m_impl->lastErrorCode,
                m_impl->connectionCreditSet, m_impl->connectionCredit,
                m_impl->sendCredits.size(), CountZeroCredits(m_impl->sendCredits),
                m_impl->transportBlocked, m_impl->pendingDataActions.size(),
                m_impl->pendingControlActions.size(),
                m_impl->pendingTerminalActions.size(),
                m_impl->pendingDataBytes,
                m_impl->pendingBodyBudgetBytes,
                m_impl->pendingReceiveCreditActions.size(),
                CountProvisionalStreams(m_impl->streams) };
        }

        Result<Http3QuicSendCreditSnapshot> Http3QuicAdapter::SendCredit(
            std::uint64_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream.GetStatus();

            const auto entry = m_impl->sendCredits.find(streamId);
            const bool streamCreditActive = entry != m_impl->sendCredits.end();
            const std::uint64_t streamCredit = streamCreditActive ? entry->second : 0;
            const bool bounded = m_impl->connectionCreditSet || streamCreditActive;
            std::uint64_t availableCredit = 0;
            if (m_impl->connectionCreditSet && streamCreditActive) {
                availableCredit = std::min(m_impl->connectionCredit, streamCredit);
            } else if (m_impl->connectionCreditSet) {
                availableCredit = m_impl->connectionCredit;
            } else if (streamCreditActive) {
                availableCredit = streamCredit;
            }
            return Http3QuicSendCreditSnapshot{
                m_impl->connectionCreditSet,
                m_impl->connectionCredit,
                streamCreditActive,
                streamCredit,
                bounded,
                bounded && availableCredit == 0,
                availableCredit
            };
        }

        Result<Http3QuicPendingDataSnapshot> Http3QuicAdapter::PendingData(
            std::uint64_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto stream = m_impl->RequireStreamId(streamId);
            if (!stream.IsOk()) return stream.GetStatus();

            Http3QuicPendingDataSnapshot result;
            for (const auto& entry : m_impl->pendingDataActions) {
                if (entry.second.streamId != streamId) continue;
                ++result.actions;
                result.bytes += entry.second.bytes;
                if (entry.second.bodyBudgetBound) {
                    result.bodyBudgetBytes += entry.second.bytes;
                }
            }
            return result;
        }

        Result<Http3QuicReceiveCreditSnapshot> Http3QuicAdapter::ReceiveCredit(
            std::uint64_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP/3 QUIC adapter is moved-from");
            auto validStream = m_impl->RequireStreamId(streamId);
            if (!validStream.IsOk()) return validStream.GetStatus();

            const auto& connection = m_impl->receiveConnectionCredit;
            const auto stream = m_impl->receiveStreamCredits.find(streamId);
            return Http3QuicReceiveCreditSnapshot{
                connection.set,
                connection.limit,
                connection.pendingActionId != 0,
                connection.pendingLimit,
                stream != m_impl->receiveStreamCredits.end()
                    && stream->second.set,
                stream == m_impl->receiveStreamCredits.end()
                    ? 0 : stream->second.limit,
                stream != m_impl->receiveStreamCredits.end()
                    && stream->second.pendingActionId != 0,
                stream == m_impl->receiveStreamCredits.end()
                    ? 0 : stream->second.pendingLimit
            };
        }

        Http3QuicErrorContext Http3QuicAdapter::LastErrorContext() const noexcept {
            if (!m_impl) return Http3QuicErrorContext{};
            auto context = m_impl->errorContext;
            context.deadlineExpired = m_impl->deadlineExpired;
            return context;
        }

        HttpErrorContext Http3QuicAdapter::LastHttpErrorContext() const noexcept {
            if (!m_impl) return {};
            const auto context = LastErrorContext();
            if (!context.valid && !context.deadlineExpired
                && !context.transportFeedbackValid && m_impl->failure.IsOk()) {
                return {};
            }

            HttpErrorOrigin origin = HttpErrorOrigin::Protocol;
            if (context.applicationError) {
                origin = HttpErrorOrigin::Application;
            } else if (context.transportFeedbackValid) {
                origin = HttpErrorOrigin::Transport;
            } else if (context.deadlineExpired) {
                origin = HttpErrorOrigin::Lifecycle;
            } else if (m_impl->failure.Code() == StatusCode::ResourceExhausted) {
                origin = HttpErrorOrigin::Resource;
            }
            auto statusCode = m_impl->failure.Code();
            if (context.deadlineExpired && statusCode == StatusCode::Ok) {
                statusCode = StatusCode::DeadlineExceeded;
            }
            return {
                true,
                HttpVersion::Http3,
                context.streamId == 0
                    ? HttpErrorScope::Connection : HttpErrorScope::Stream,
                origin,
                context.streamId,
                context.valid
                    ? HttpErrorUnitKind::Event : HttpErrorUnitKind::None,
                static_cast<std::uint64_t>(context.eventKind),
                0,
                statusCode
            };
        }

        Status Http3QuicAdapter::LastError() const {
            return m_impl == nullptr
                ? Status::Internal(u"HTTP/3 QUIC adapter is moved-from")
                : m_impl->failure;
        }

        void Http3QuicAdapter::Reset() noexcept {
            if (!m_impl) return;
            m_impl->state = Http3QuicAdapterState::AwaitingHandshake;
            m_impl->failure = {};
            m_impl->deadline = LikesProgram::Time::Deadline::Infinite();
            m_impl->streams.clear();
            m_impl->sendCredits.clear();
            m_impl->connectionCreditSet = false;
            m_impl->connectionCredit = 0;
            m_impl->streamCreditVersions.clear();
            m_impl->connectionCreditVersion = 0;
            m_impl->transportBlocked = false;
            m_impl->nextActionId = 1;
            m_impl->ReleaseBodyBudgetReservations();
            m_impl->pendingDataActions.clear();
            m_impl->pendingDataBytes = 0;
            m_impl->pendingBodyBudgetBytes = 0;
            m_impl->pendingControlActions.clear();
            m_impl->pendingTerminalActions.clear();
            m_impl->receiveConnectionCredit = {};
            m_impl->receiveStreamCredits.clear();
            m_impl->pendingReceiveCreditActions.clear();
            m_impl->eventCount = 0;
            m_impl->actionCount = 0;
            m_impl->lastErrorCode = 0;
            m_impl->errorContext = {};
            m_impl->deadlineExpired = false;
        }
    }
}
