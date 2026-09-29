#include <LikesProgram/Http/Http3Control.hpp>

#include <unordered_set>
#include <utility>

namespace {
    LikesProgram::Result<std::uint64_t> ParseSingleHttp3VarInt(
        const std::vector<std::uint8_t>& payload,
        const char16_t* message) {
        auto parsed = LikesProgram::Http::ParseHttp3VarInt(
            payload.data(), payload.size());
        if (!parsed.IsOk()) return parsed.PropagateFailure<std::uint64_t>();
        if (parsed.Value().second != payload.size()) {
            return LikesProgram::Status::InvalidArgument(message);
        }
        return parsed.Value().first;
    }
}

namespace LikesProgram {
    namespace Http {
        Result<std::vector<Http3Setting>> ParseHttp3Settings(
            const std::vector<std::uint8_t>& payload) {
            std::vector<Http3Setting> settings;
            std::unordered_set<std::uint64_t> ids;
            std::size_t offset = 0;
            while (offset < payload.size()) {
                auto id = ParseHttp3VarInt(
                    payload.data() + offset, payload.size() - offset);
                if (!id.IsOk()) return id.PropagateFailure<std::vector<Http3Setting>>();
                offset += id.Value().second;
                auto value = ParseHttp3VarInt(
                    payload.data() + offset, payload.size() - offset);
                if (!value.IsOk()) {
                    return value.PropagateFailure<std::vector<Http3Setting>>();
                }
                offset += value.Value().second;
                if (!ids.insert(id.Value().first).second) {
                    return Status::InvalidArgument(
                        u"HTTP/3 SETTINGS contains a duplicate identifier");
                }
                settings.push_back(Http3Setting{ id.Value().first, value.Value().first });
            }
            return settings;
        }

        Result<std::vector<std::uint8_t>> BuildHttp3Settings(
            const std::vector<Http3Setting>& settings) {
            std::unordered_set<std::uint64_t> ids;
            std::vector<std::uint8_t> output;
            for (const auto& setting : settings) {
                if (!ids.insert(setting.id).second) {
                    return Status::InvalidArgument(
                        u"HTTP/3 SETTINGS contains a duplicate identifier");
                }
                auto id = BuildHttp3VarInt(setting.id);
                if (!id.IsOk()) return id.GetStatus();
                auto value = BuildHttp3VarInt(setting.value);
                if (!value.IsOk()) return value.GetStatus();
                output.insert(output.end(), id.Value().begin(), id.Value().end());
                output.insert(output.end(), value.Value().begin(), value.Value().end());
            }
            return output;
        }

        Result<std::vector<std::uint8_t>> BuildHttp3ControlStreamType() {
            return BuildHttp3VarInt(kHttp3ControlStreamType);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3ControlStreamInitialBytes(
            const std::vector<Http3Setting>& settings) {
            const auto streamType = BuildHttp3ControlStreamType();
            if (!streamType.IsOk()) return streamType.GetStatus();
            const auto settingsPayload = BuildHttp3Settings(settings);
            if (!settingsPayload.IsOk()) return settingsPayload.GetStatus();
            const auto settingsFrame = BuildHttp3Frame(Http3Frame{
                static_cast<std::uint64_t>(Http3FrameType::Settings),
                settingsPayload.Value() });
            if (!settingsFrame.IsOk()) return settingsFrame.GetStatus();
            std::vector<std::uint8_t> output;
            output.reserve(streamType.Value().size() + settingsFrame.Value().size());
            output.insert(output.end(), streamType.Value().begin(), streamType.Value().end());
            output.insert(output.end(), settingsFrame.Value().begin(), settingsFrame.Value().end());
            return output;
        }

        struct Http3ControlStream::Impl {
            explicit Impl(std::size_t configuredMaxSettings)
                : maxSettings(configuredMaxSettings) {
                if (maxSettings == 0) {
                    state = Http3ControlStreamState::Failed;
                    lastError = Status::InvalidArgument(
                        u"HTTP/3 control stream settings limit must be positive");
                }
            }

            Result<void> Fail(const Status& failure) {
                if (lastError.IsOk()) lastError = failure;
                state = Http3ControlStreamState::Failed;
                return lastError;
            }

            const std::size_t maxSettings;
            Http3ControlStreamState state =
                Http3ControlStreamState::AwaitingStreamType;
            Status lastError;
            bool streamTypeAccepted = false;
            bool settingsReceived = false;
            std::vector<Http3Setting> settings;
            bool goawayReceived = false;
            std::uint64_t goawayId = 0;
            bool maxPushIdReceived = false;
            std::uint64_t maxPushId = 0;
            std::size_t cancelPushCount = 0;
            std::uint64_t lastCancelPushId = 0;
        };

        Http3ControlStream::Http3ControlStream(std::size_t maxSettings)
            : m_impl(std::make_unique<Impl>(maxSettings)) { }

        Http3ControlStream::~Http3ControlStream() = default;

        Http3ControlStream::Http3ControlStream(Http3ControlStream&&) noexcept = default;

        Http3ControlStream& Http3ControlStream::operator=(
            Http3ControlStream&&) noexcept = default;

        Result<void> Http3ControlStream::AcceptStreamType(std::uint64_t streamType) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 control stream is moved-from");
            if (!m_impl->lastError.IsOk()) return m_impl->lastError;
            if (m_impl->streamTypeAccepted) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 control stream type was already accepted");
            if (streamType != kHttp3ControlStreamType) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 unidirectional stream type is not control"));
            }
            m_impl->streamTypeAccepted = true;
            m_impl->state = Http3ControlStreamState::AwaitingSettings;
            return {};
        }

        Result<void> Http3ControlStream::Feed(const Http3Frame& frame) {
            if (!m_impl) return Status::Internal(
                u"HTTP/3 control stream is moved-from");
            if (!m_impl->lastError.IsOk()) return m_impl->lastError;
            if (!m_impl->streamTypeAccepted) return Status(StatusCode::FailedPrecondition,
                u"HTTP/3 control stream type must be accepted first");
            if (!m_impl->settingsReceived
                && frame.type != static_cast<std::uint64_t>(Http3FrameType::Settings)) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 control stream must begin with SETTINGS"));
            }

            switch (static_cast<Http3FrameType>(frame.type)) {
            case Http3FrameType::Settings: {
                if (m_impl->settingsReceived) return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 control stream contains duplicate SETTINGS"));
                auto parsed = ParseHttp3Settings(frame.payload);
                if (!parsed.IsOk()) return m_impl->Fail(parsed.GetStatus());
                if (parsed.Value().size() > m_impl->maxSettings) {
                    return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 control stream SETTINGS exceeds configured limit"));
                }
                m_impl->settings = parsed.MoveValue();
                m_impl->settingsReceived = true;
                m_impl->state = Http3ControlStreamState::Open;
                return {};
            }
            case Http3FrameType::Goaway: {
                auto id = ParseSingleHttp3VarInt(
                    frame.payload, u"HTTP/3 GOAWAY payload must contain one varint");
                if (!id.IsOk()) return m_impl->Fail(id.GetStatus());
                if (m_impl->goawayReceived && id.Value() > m_impl->goawayId) {
                    return m_impl->Fail(Status::InvalidArgument(
                        u"HTTP/3 GOAWAY identifier must not increase"));
                }
                m_impl->goawayReceived = true;
                m_impl->goawayId = id.Value();
                return {};
            }
            case Http3FrameType::MaxPushId: {
                auto id = ParseSingleHttp3VarInt(
                    frame.payload, u"HTTP/3 MAX_PUSH_ID payload must contain one varint");
                if (!id.IsOk()) return m_impl->Fail(id.GetStatus());
                if (m_impl->maxPushIdReceived && id.Value() < m_impl->maxPushId) {
                    return m_impl->Fail(Status::InvalidArgument(
                        u"HTTP/3 MAX_PUSH_ID identifier must not decrease"));
                }
                m_impl->maxPushIdReceived = true;
                m_impl->maxPushId = id.Value();
                return {};
            }
            case Http3FrameType::CancelPush: {
                auto id = ParseSingleHttp3VarInt(
                    frame.payload, u"HTTP/3 CANCEL_PUSH payload must contain one varint");
                if (!id.IsOk()) return m_impl->Fail(id.GetStatus());
                ++m_impl->cancelPushCount;
                m_impl->lastCancelPushId = id.Value();
                return {};
            }
            case Http3FrameType::Data:
            case Http3FrameType::Headers:
            case Http3FrameType::PushPromise:
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 control stream received a request stream frame"));
            default:
                // Unknown extension frames are permitted after SETTINGS.
                return {};
            }
        }

        const std::vector<Http3Setting>& Http3ControlStream::Settings() const noexcept {
            static const std::vector<Http3Setting> empty;
            return m_impl ? m_impl->settings : empty;
        }

        Http3ControlStreamSnapshot Http3ControlStream::Snapshot() const noexcept {
            if (!m_impl) return Http3ControlStreamSnapshot{
                Http3ControlStreamState::Failed };
            return Http3ControlStreamSnapshot{
                m_impl->state,
                m_impl->streamTypeAccepted,
                m_impl->settingsReceived,
                m_impl->settings.size(),
                m_impl->goawayReceived,
                m_impl->goawayId,
                m_impl->maxPushIdReceived,
                m_impl->maxPushId,
                m_impl->cancelPushCount,
                m_impl->lastCancelPushId
            };
        }

        Http3ControlStreamState Http3ControlStream::State() const noexcept {
            return m_impl == nullptr
                ? Http3ControlStreamState::Failed : m_impl->state;
        }

        Status Http3ControlStream::LastError() const {
            return m_impl == nullptr
                ? Status::Internal(u"HTTP/3 control stream is moved-from")
                : m_impl->lastError;
        }

        void Http3ControlStream::Reset() noexcept {
            if (!m_impl) return;
            m_impl->state = Http3ControlStreamState::AwaitingStreamType;
            m_impl->lastError = Status();
            m_impl->streamTypeAccepted = false;
            m_impl->settingsReceived = false;
            m_impl->settings.clear();
            m_impl->goawayReceived = false;
            m_impl->goawayId = 0;
            m_impl->maxPushIdReceived = false;
            m_impl->maxPushId = 0;
            m_impl->cancelPushCount = 0;
            m_impl->lastCancelPushId = 0;
            if (m_impl->maxSettings == 0) {
                m_impl->state = Http3ControlStreamState::Failed;
                m_impl->lastError = Status::InvalidArgument(
                    u"HTTP/3 control stream settings limit must be positive");
            }
        }

        struct Http3ControlStreamWireDecoder::Impl {
            explicit Impl(std::size_t maxSettings,
                Http3ControlStreamWireLimits wireLimits)
                : control(maxSettings), limits(wireLimits) { }

            Result<void> Fail(const Status& failure,
                Http3ErrorCode code = Http3ErrorCode::GeneralProtocolError) {
                if (status.IsOk()) {
                    status = failure;
                    failureCode = code;
                }
                return status;
            }

            Http3ControlStream control;
            Http3ControlStreamWireLimits limits;
            std::vector<std::uint8_t> pending;
            std::size_t offset = 0;
            Status status;
            Http3ErrorCode failureCode = Http3ErrorCode::GeneralProtocolError;
            bool typeAccepted = false;
            bool finished = false;

            std::size_t PendingBytes() const noexcept {
                return pending.size() - offset;
            }

            void Compact() {
                if (offset == 0) return;
                if (offset == pending.size()) {
                    pending.clear();
                    offset = 0;
                    return;
                }
                if (offset >= 4096 && offset * 2 >= pending.size()) {
                    pending.erase(pending.begin(), pending.begin() + offset);
                    offset = 0;
                }
            }
        };

        Http3ControlStreamWireDecoder::Http3ControlStreamWireDecoder(
            std::size_t maxSettings,
            Http3ControlStreamWireLimits wireLimits)
            : m_impl(std::make_unique<Impl>(maxSettings, wireLimits)) {
            if (wireLimits.maxPendingBytes == 0
                || wireLimits.maxFramePayloadBytes == 0) {
                m_impl->status = Status::InvalidArgument(
                    u"HTTP/3 control stream wire limits must be non-zero");
            }
        }

        Http3ControlStreamWireDecoder::~Http3ControlStreamWireDecoder() = default;

        Http3ControlStreamWireDecoder::Http3ControlStreamWireDecoder(
            Http3ControlStreamWireDecoder&&) noexcept = default;

        Http3ControlStreamWireDecoder& Http3ControlStreamWireDecoder::operator=(
            Http3ControlStreamWireDecoder&&) noexcept = default;

        Result<void> Http3ControlStreamWireDecoder::Feed(
            const std::uint8_t* data,
            std::size_t size,
            bool endStream) {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 control stream wire decoder is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->finished) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 control stream wire decoder received data after FIN");
            }
            if (data == nullptr && size != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 control stream wire data is null"));
            }
            if (size > m_impl->limits.maxPendingBytes
                || m_impl->PendingBytes() > m_impl->limits.maxPendingBytes - size) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 control stream wire pending limit exceeded"));
            }
            if (size != 0) {
                m_impl->pending.insert(m_impl->pending.end(), data, data + size);
            }

            while (m_impl->PendingBytes() != 0) {
                const auto available = m_impl->PendingBytes();
                const auto* bytes = m_impl->pending.data() + m_impl->offset;
                if (!m_impl->typeAccepted) {
                    const auto width = std::size_t{ 1 } << (bytes[0] >> 6);
                    if (available < width) break;
                    auto type = ParseHttp3VarInt(bytes, available);
                    if (!type.IsOk()) return m_impl->Fail(type.GetStatus());
                    if (type.Value().first != kHttp3ControlStreamType) {
                        return m_impl->Fail(Status::InvalidArgument(
                            u"HTTP/3 unidirectional stream type is not control"));
                    }
                    auto accepted = m_impl->control.AcceptStreamType(type.Value().first);
                    if (!accepted.IsOk()) return m_impl->Fail(accepted.GetStatus());
                    m_impl->offset += type.Value().second;
                    m_impl->typeAccepted = true;
                    m_impl->Compact();
                    continue;
                }

                const auto typeWidth = std::size_t{ 1 } << (bytes[0] >> 6);
                if (available < typeWidth) break;
                auto type = ParseHttp3VarInt(bytes, available);
                if (!type.IsOk()) return m_impl->Fail(type.GetStatus());
                const auto lengthOffset = type.Value().second;
                if (available <= lengthOffset) break;
                const auto lengthWidth = std::size_t{ 1 }
                    << (bytes[lengthOffset] >> 6);
                if (available < lengthOffset + lengthWidth) break;
                auto length = ParseHttp3VarInt(
                    bytes + lengthOffset, available - lengthOffset);
                if (!length.IsOk()) return m_impl->Fail(length.GetStatus());
                if (length.Value().first > m_impl->limits.maxFramePayloadBytes) {
                    return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 control stream wire frame limit exceeded"));
                }
                const auto payloadOffset = lengthOffset + length.Value().second;
                if (payloadOffset > available
                    || length.Value().first > available - payloadOffset) {
                    break;
                }
                const auto frameBytes = payloadOffset
                    + static_cast<std::size_t>(length.Value().first);
                Http3Frame frame;
                frame.type = type.Value().first;
                frame.payload.assign(bytes + payloadOffset, bytes + frameBytes);
                auto fed = m_impl->control.Feed(frame);
                if (!fed.IsOk()) return m_impl->Fail(fed.GetStatus());
                m_impl->offset += frameBytes;
                m_impl->Compact();
            }

            if (!endStream) return {};
            if (m_impl->PendingBytes() != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 control stream FIN truncated a frame"));
            }
            m_impl->finished = true;
            return m_impl->Fail(Status::InvalidArgument(
                u"HTTP/3 control stream closed before connection shutdown"),
                Http3ErrorCode::ClosedCriticalStream);
        }

        Result<void> Http3ControlStreamWireDecoder::Finish() {
            return Feed(nullptr, 0, true);
        }

        Http3ControlStreamWireLimits
            Http3ControlStreamWireDecoder::WireLimits() const noexcept {
            return m_impl ? m_impl->limits : Http3ControlStreamWireLimits{};
        }

        std::size_t Http3ControlStreamWireDecoder::PendingBytes() const noexcept {
            return m_impl ? m_impl->PendingBytes() : 0;
        }

        Http3ControlStreamSnapshot
            Http3ControlStreamWireDecoder::Snapshot() const noexcept {
            return m_impl ? m_impl->control.Snapshot()
                : Http3ControlStreamSnapshot{ Http3ControlStreamState::Failed };
        }

        const std::vector<Http3Setting>&
            Http3ControlStreamWireDecoder::Settings() const noexcept {
            static const std::vector<Http3Setting> empty;
            return m_impl ? m_impl->control.Settings() : empty;
        }

        Status Http3ControlStreamWireDecoder::LastError() const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 control stream wire decoder is moved-from");
            }
            return m_impl->status.IsOk() ? m_impl->control.LastError()
                : m_impl->status;
        }

        Result<Http3ControlStreamQuicActions>
            Http3ControlStreamWireDecoder::FailureActions() const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 control stream wire decoder is moved-from");
            }
            if (LastError().IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 control stream wire decoder has no failure");
            }
            return Http3ControlStreamQuicActions{
                static_cast<std::uint64_t>(m_impl->failureCode), true };
        }

        void Http3ControlStreamWireDecoder::Reset() noexcept {
            if (!m_impl) return;
            m_impl->control.Reset();
            m_impl->pending.clear();
            m_impl->offset = 0;
            m_impl->status = Status::OkStatus();
            m_impl->failureCode = Http3ErrorCode::GeneralProtocolError;
            m_impl->typeAccepted = false;
            m_impl->finished = false;
            if (m_impl->limits.maxPendingBytes == 0
                || m_impl->limits.maxFramePayloadBytes == 0) {
                m_impl->status = Status::InvalidArgument(
                    u"HTTP/3 control stream wire limits must be non-zero");
            }
        }
    }
}
