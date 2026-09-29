#include <LikesProgram/Http/Http2.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace {
    constexpr std::size_t kHttp2FrameHeaderSize = 9;       // HTTP/2 帧头固定 9 字节
    constexpr std::uint32_t kHttp2MaxFrameLength = 0x00FFFFFF; // length 字段为 24-bit
    constexpr std::int64_t kHttp2MaxWindowSize = 0x7FFFFFFF;

    // 按网络字节序读取 24-bit 长度字段。
    std::uint32_t ReadUint24(const std::uint8_t* data) noexcept {
        return (static_cast<std::uint32_t>(data[0]) << 16)
            | (static_cast<std::uint32_t>(data[1]) << 8)
            | static_cast<std::uint32_t>(data[2]);
    }

    // 按网络字节序读取 31-bit stream id，并清除最高保留位。
    std::uint32_t ReadStreamId(const std::uint8_t* data) noexcept {
        return ((static_cast<std::uint32_t>(data[0] & 0x7F) << 24)
            | (static_cast<std::uint32_t>(data[1]) << 16)
            | (static_cast<std::uint32_t>(data[2]) << 8)
            | static_cast<std::uint32_t>(data[3]));
    }

    // 写入 24-bit 长度字段，调用方已保证范围合法。
    void WriteUint24(std::vector<std::uint8_t>& output, std::uint32_t value) {
        output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        output.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

    // 写入 31-bit stream id，最高保留位始终写 0。
    void WriteStreamId(std::vector<std::uint8_t>& output, std::uint32_t streamId) {
        output.push_back(static_cast<std::uint8_t>((streamId >> 24) & 0x7F));
        output.push_back(static_cast<std::uint8_t>((streamId >> 16) & 0xFF));
        output.push_back(static_cast<std::uint8_t>((streamId >> 8) & 0xFF));
        output.push_back(static_cast<std::uint8_t>(streamId & 0xFF));
    }

    void WriteUint16(std::vector<std::uint8_t>& output, std::uint16_t value) {
        output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        output.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

    std::uint16_t ReadUint16(const std::uint8_t* data) noexcept {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
    }

    std::uint32_t ReadUint32(const std::uint8_t* data) noexcept {
        return (static_cast<std::uint32_t>(data[0]) << 24)
            | (static_cast<std::uint32_t>(data[1]) << 16)
            | (static_cast<std::uint32_t>(data[2]) << 8)
            | static_cast<std::uint32_t>(data[3]);
    }

    void WriteUint32(std::vector<std::uint8_t>& output, std::uint32_t value) {
        output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
        output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        output.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }
}

namespace LikesProgram {
    namespace Http {
        Result<Http2BodyCancellationAction> MapHttp2BodyCancellation(
            HttpBodyCancelReason reason) {
            switch (reason) {
            case HttpBodyCancelReason::Application:
            case HttpBodyCancelReason::DeadlineExceeded:
                return Http2BodyCancellationAction{
                    Http2BodyCancellationActionKind::ResetStream,
                    Http2ErrorCode::Cancel };
            case HttpBodyCancelReason::ProtocolError:
                return Http2BodyCancellationAction{
                    Http2BodyCancellationActionKind::ResetStream,
                    Http2ErrorCode::ProtocolError };
            case HttpBodyCancelReason::PeerReset:
                return Http2BodyCancellationAction{
                    Http2BodyCancellationActionKind::AlreadyHandled,
                    Http2ErrorCode::NoError };
            }
            return Status::InvalidArgument(
                u"HTTP/2 body cancellation reason is not supported");
        }

        bool IsHttp2ConnectionPreface(const std::uint8_t* data, std::size_t size) noexcept {
            if (size != kHttp2ConnectionPreface.size() || data == nullptr) return false;

            const auto* expected = reinterpret_cast<const std::uint8_t*>(
                kHttp2ConnectionPreface.data());              // 固定前言字节视图
            return std::equal(expected, expected + kHttp2ConnectionPreface.size(), data);
        }

        std::vector<std::uint8_t> BuildHttp2ConnectionPreface() {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(
                kHttp2ConnectionPreface.data());              // 前言起始字节
            const auto* end = begin + kHttp2ConnectionPreface.size(); // 前言尾后字节
            return std::vector<std::uint8_t>(begin, end);
        }

        Result<Http2Frame> ParseHttp2Frame(const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size < kHttp2FrameHeaderSize) {
                return Status::InvalidArgument(u"HTTP/2 frame header is incomplete");
            }

            const std::uint32_t length = ReadUint24(data);    // payload 长度字段
            const std::size_t expectedSize = kHttp2FrameHeaderSize
                + static_cast<std::size_t>(length);           // 完整帧总长度
            if (size != expectedSize) {
                return Status::InvalidArgument(u"HTTP/2 frame size mismatch");
            }

            Http2Frame frame;                                 // 解析后的帧对象
            frame.length = length;
            frame.type = data[3];
            frame.flags = data[4];
            frame.streamId = ReadStreamId(data + 5);
            frame.payload.assign(data + kHttp2FrameHeaderSize, data + expectedSize);
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildHttp2Frame(const Http2Frame& frame) {
            if (frame.payload.size() > kHttp2MaxFrameLength) {
                return Status::InvalidArgument(u"HTTP/2 frame payload too large");
            }
            if ((frame.streamId & 0x80000000U) != 0) {
                return Status::InvalidArgument(u"HTTP/2 stream id uses reserved bit");
            }

            const auto length = static_cast<std::uint32_t>(frame.payload.size()); // 序列化使用真实 payload 长度
            std::vector<std::uint8_t> output;                  // 完整帧二进制输出
            output.reserve(kHttp2FrameHeaderSize + frame.payload.size());
            WriteUint24(output, length);
            output.push_back(frame.type);
            output.push_back(frame.flags);
            WriteStreamId(output, frame.streamId);
            output.insert(output.end(), frame.payload.begin(), frame.payload.end());
            return output;
        }

        Result<std::vector<Http2Setting>> ParseHttp2Settings(
            const std::vector<std::uint8_t>& payload) {
            if (payload.size() % 6 != 0) {
                return Status::InvalidArgument(u"HTTP/2 SETTINGS payload is not 6-byte aligned");
            }

            std::vector<Http2Setting> settings;
            settings.reserve(payload.size() / 6);
            for (std::size_t offset = 0; offset < payload.size(); offset += 6) {
                const auto id = ReadUint16(payload.data() + offset);
                if (std::any_of(settings.begin(), settings.end(), [id](const Http2Setting& setting) {
                    return setting.id == id;
                })) {
                    return Status::InvalidArgument(u"HTTP/2 SETTINGS frame repeats an identifier");
                }
                settings.push_back(Http2Setting{ id, ReadUint32(payload.data() + offset + 2) });
            }
            return settings;
        }

        Result<std::vector<std::uint8_t>> BuildHttp2Settings(
            const std::vector<Http2Setting>& settings) {
            std::vector<std::uint8_t> output;
            output.reserve(settings.size() * 6);
            for (std::size_t index = 0; index < settings.size(); ++index) {
                const auto& setting = settings[index];
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (settings[prior].id == setting.id) {
                        return Status::InvalidArgument(
                            u"HTTP/2 SETTINGS frame repeats an identifier");
                    }
                }
                WriteUint16(output, setting.id);
                WriteUint32(output, setting.value);
            }
            return output;
        }

        struct Http2Session::Impl {
            struct Stream {
                Http2StreamInfo info;
            };

            explicit Impl(bool clientValue, Http2SessionLimits sessionLimits)
                : client(clientValue), limits(sessionLimits),
                  peerInitialWindow(sessionLimits.initialWindowSize),
                  peerMaxConcurrentStreams(sessionLimits.maxConcurrentStreams),
                  connectionSendWindow(sessionLimits.initialWindowSize),
                  connectionReceiveWindow(sessionLimits.initialWindowSize) { }

            bool IsLocalStreamId(std::uint32_t streamId) const noexcept {
                return (streamId & 1U) != 0U ? client : !client;
            }

            Status RecordFailure(
                Status status,
                HttpErrorOrigin origin,
                HttpErrorScope scope,
                std::uint32_t streamId = 0,
                bool frameTypeValid = false,
                Http2FrameType frameType = Http2FrameType::Data) {
                lastStatus = status;
                errorContext = {
                    true,
                    HttpVersion::Http2,
                    scope,
                    origin,
                    streamId,
                    frameTypeValid
                        ? HttpErrorUnitKind::Frame : HttpErrorUnitKind::None,
                    static_cast<std::uint64_t>(frameType),
                    0,
                    status.Code()
                };
                return lastStatus;
            }

            Result<void> FailConnection(
                Status status,
                Http2ErrorCode code,
                std::uint32_t streamId = 0,
                bool frameTypeValid = false,
                Http2FrameType frameType = Http2FrameType::Data,
                HttpErrorOrigin origin = HttpErrorOrigin::Protocol) {
                if (failure.IsOk()) {
                    failure = RecordFailure(
                        status,
                        origin,
                        HttpErrorScope::Connection,
                        streamId,
                        frameTypeValid,
                        frameType);
                    lastError = code;
                    state = Http2SessionState::Closed;
                }
                return failure;
            }

            Result<void> RequireUsable() {
                if (!failure.IsOk()) return failure;
                if (state == Http2SessionState::Closed) {
                    return RecordFailure(
                        Status(StatusCode::FailedPrecondition,
                            u"HTTP/2 session is closed"),
                        HttpErrorOrigin::Lifecycle,
                        HttpErrorScope::Connection);
                }
                return {};
            }

            std::size_t ActiveStreams(bool local) const noexcept {
                std::size_t count = 0;
                for (const auto& entry : streams) {
                    if (entry.second.info.localInitiated == local
                        && entry.second.info.state != Http2StreamState::Closed) {
                        ++count;
                    }
                }
                return count;
            }

            bool client = true;
            Http2SessionLimits limits;
            Http2SessionState state = Http2SessionState::Open;
            Status failure;
            Status lastStatus;
            HttpErrorContext errorContext;
            Http2ErrorCode lastError = Http2ErrorCode::NoError;
            std::unordered_map<std::uint32_t, Stream> streams;
            std::uint32_t highestLocalStream = 0;
            std::uint32_t highestRemoteStream = 0;
            std::uint32_t continuationStream = 0;
            std::uint32_t peerInitialWindow = 65535;
            std::uint32_t peerMaxConcurrentStreams = 100;
            std::uint32_t peerMaxFrameSize = 16384;
            std::int64_t connectionSendWindow = 65535;
            std::int64_t connectionReceiveWindow = 65535;
            std::uint32_t localGoawayLastStream = 0x7FFFFFFFU;
            std::uint32_t peerGoawayLastStream = 0x7FFFFFFFU;
            std::uint32_t settingsAckPendingCount = 0;
        };

        Http2Session::Http2Session(bool client, Http2SessionLimits limits)
            : m_impl(std::make_unique<Impl>(client, limits)) {
            if (limits.initialWindowSize > kHttp2MaxWindowSize
                || limits.maxFrameSize < 16384
                || limits.maxFrameSize > kHttp2MaxFrameLength) {
                m_impl->failure = Status::InvalidArgument(
                    u"HTTP/2 session limits are out of range");
                m_impl->RecordFailure(
                    m_impl->failure,
                    HttpErrorOrigin::Application,
                    HttpErrorScope::Connection);
                m_impl->lastError = Http2ErrorCode::ProtocolError;
                m_impl->state = Http2SessionState::Closed;
            }
        }

        Http2Session::~Http2Session() = default;

        Http2Session::Http2Session(Http2Session&&) noexcept = default;

        Http2Session& Http2Session::operator=(Http2Session&&) noexcept = default;

        Result<void> Http2Session::ApplySettings(
            const std::vector<Http2Setting>& settings) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;

            std::uint32_t newInitialWindow = m_impl->peerInitialWindow;
            std::uint32_t newMaxConcurrent = m_impl->peerMaxConcurrentStreams;
            std::uint32_t newMaxFrame = m_impl->peerMaxFrameSize;
            for (std::size_t index = 0; index < settings.size(); ++index) {
                const auto& setting = settings[index];
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (settings[prior].id == setting.id) {
                        return m_impl->FailConnection(
                            Status::InvalidArgument(u"HTTP/2 SETTINGS frame repeats an identifier"),
                            Http2ErrorCode::ProtocolError,
                            0, true, Http2FrameType::Settings);
                    }
                }
                switch (static_cast<Http2SettingId>(setting.id)) {
                case Http2SettingId::EnablePush:
                    if (setting.value > 1) {
                        return m_impl->FailConnection(
                            Status::InvalidArgument(u"HTTP/2 ENABLE_PUSH must be 0 or 1"),
                            Http2ErrorCode::ProtocolError,
                            0, true, Http2FrameType::Settings);
                    }
                    break;
                case Http2SettingId::InitialWindowSize:
                    if (setting.value > kHttp2MaxWindowSize) {
                        return m_impl->FailConnection(
                            Status::InvalidArgument(u"HTTP/2 INITIAL_WINDOW_SIZE is too large"),
                            Http2ErrorCode::FlowControlError,
                            0, true, Http2FrameType::Settings);
                    }
                    newInitialWindow = setting.value;
                    break;
                case Http2SettingId::MaxFrameSize:
                    if (setting.value < 16384 || setting.value > kHttp2MaxFrameLength) {
                        return m_impl->FailConnection(
                            Status::InvalidArgument(u"HTTP/2 MAX_FRAME_SIZE is out of range"),
                            Http2ErrorCode::ProtocolError,
                            0, true, Http2FrameType::Settings);
                    }
                    newMaxFrame = setting.value;
                    break;
                case Http2SettingId::MaxConcurrentStreams:
                    newMaxConcurrent = setting.value;
                    break;
                case Http2SettingId::HeaderTableSize:
                case Http2SettingId::MaxHeaderListSize:
                default:
                    break;
                }
            }

            const auto delta = static_cast<std::int64_t>(newInitialWindow)
                - static_cast<std::int64_t>(m_impl->peerInitialWindow);
            for (const auto& entry : m_impl->streams) {
                if (entry.second.info.state == Http2StreamState::Closed) continue;
                const auto candidate = entry.second.info.sendWindow + delta;
                if (candidate > kHttp2MaxWindowSize) {
                    return m_impl->FailConnection(
                        Status::InvalidArgument(u"HTTP/2 stream send window overflows"),
                        Http2ErrorCode::FlowControlError,
                        entry.first, true, Http2FrameType::Settings);
                }
            }
            for (auto& entry : m_impl->streams) {
                if (entry.second.info.state != Http2StreamState::Closed) {
                    entry.second.info.sendWindow += delta;
                }
            }
            m_impl->peerInitialWindow = newInitialWindow;
            m_impl->peerMaxConcurrentStreams = newMaxConcurrent;
            m_impl->peerMaxFrameSize = newMaxFrame;
            if (m_impl->settingsAckPendingCount == std::numeric_limits<std::uint32_t>::max()) {
                return m_impl->FailConnection(
                    Status(StatusCode::OutOfRange,
                        u"HTTP/2 SETTINGS ACK count overflows"),
                    Http2ErrorCode::InternalError,
                    0, true, Http2FrameType::Settings,
                    HttpErrorOrigin::Resource);
            }
            ++m_impl->settingsAckPendingCount;
            return {};
        }

        Result<void> Http2Session::AcknowledgeSettings() {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            if (m_impl->settingsAckPendingCount == 0) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 SETTINGS ACK has no pending SETTINGS"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Connection,
                    0, true, Http2FrameType::Settings);
            }
            --m_impl->settingsAckPendingCount;
            return {};
        }

        bool Http2Session::SettingsAckPending() const noexcept {
            return m_impl != nullptr && m_impl->settingsAckPendingCount != 0;
        }

        Result<void> Http2Session::OpenLocalStream(std::uint32_t streamId) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            if (m_impl->state == Http2SessionState::GoingAway) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 session is going away"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::Headers);
            }
            if (streamId == 0 || !m_impl->IsLocalStreamId(streamId)
                || streamId <= m_impl->highestLocalStream
                || m_impl->streams.contains(streamId)) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 local stream id is invalid"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::Headers,
                    HttpErrorOrigin::Application);
            }
            if (m_impl->ActiveStreams(true) >= m_impl->peerMaxConcurrentStreams) {
                return m_impl->RecordFailure(
                    Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 peer MAX_CONCURRENT_STREAMS reached"),
                    HttpErrorOrigin::Resource,
                    HttpErrorScope::Stream,
                    streamId,
                    true,
                    Http2FrameType::Headers);
            }
            Http2StreamInfo info;
            info.streamId = streamId;
            info.localInitiated = true;
            info.state = Http2StreamState::Open;
            info.sendWindow = m_impl->peerInitialWindow;
            info.receiveWindow = m_impl->limits.initialWindowSize;
            m_impl->streams.emplace(streamId, Impl::Stream{ info });
            m_impl->highestLocalStream = streamId;
            return {};
        }

        Result<void> Http2Session::ReceiveHeaders(
            std::uint32_t streamId, bool endStream, bool endHeaders) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            if (streamId == 0 || m_impl->continuationStream != 0) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 HEADERS arrived in an invalid sequence"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::Headers);
            }

            auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()) {
                if (m_impl->IsLocalStreamId(streamId)
                    || streamId <= m_impl->highestRemoteStream
                    || (m_impl->state == Http2SessionState::GoingAway
                        && streamId > m_impl->peerGoawayLastStream)) {
                    return m_impl->FailConnection(
                        Status::InvalidArgument(u"HTTP/2 peer stream id is invalid"),
                        Http2ErrorCode::ProtocolError,
                        streamId, true, Http2FrameType::Headers);
                }
                if (m_impl->ActiveStreams(false) >= m_impl->limits.maxConcurrentStreams) {
                    return m_impl->RecordFailure(
                        Status(StatusCode::ResourceExhausted,
                            u"HTTP/2 local MAX_CONCURRENT_STREAMS reached"),
                        HttpErrorOrigin::Resource,
                        HttpErrorScope::Stream,
                        streamId,
                        true,
                        Http2FrameType::Headers);
                }
                Http2StreamInfo info;
                info.streamId = streamId;
                info.localInitiated = false;
                info.state = Http2StreamState::Open;
                info.sendWindow = m_impl->peerInitialWindow;
                info.receiveWindow = m_impl->limits.initialWindowSize;
                iterator = m_impl->streams.emplace(streamId, Impl::Stream{ info }).first;
                m_impl->highestRemoteStream = streamId;
            }

            auto& info = iterator->second.info;
            if (info.state == Http2StreamState::Closed
                || info.state == Http2StreamState::HalfClosedRemote) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 HEADERS arrived after remote stream close"),
                    HttpErrorOrigin::Protocol,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::Headers);
            }
            info.headersReceived = true;
            if (endStream) {
                if (info.state == Http2StreamState::HalfClosedLocal) {
                    info.state = Http2StreamState::Closed;
                } else {
                    info.state = Http2StreamState::HalfClosedRemote;
                }
            }
            if (!endHeaders) {
                m_impl->continuationStream = streamId;
                info.headerBlockOpen = true;
            }
            return {};
        }

        Result<void> Http2Session::ContinueHeaders(
            std::uint32_t streamId, bool endHeaders) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            if (m_impl->continuationStream != streamId || streamId == 0) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 CONTINUATION stream does not match HEADERS"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::Continuation);
            }
            auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()) {
                return m_impl->FailConnection(
                    Status::Internal(u"HTTP/2 continuation stream disappeared"),
                    Http2ErrorCode::InternalError,
                    streamId, true, Http2FrameType::Continuation,
                    HttpErrorOrigin::Lifecycle);
            }
            if (endHeaders) {
                m_impl->continuationStream = 0;
                iterator->second.info.headerBlockOpen = false;
            }
            return {};
        }

        Result<void> Http2Session::SendData(
            std::uint32_t streamId, std::uint32_t byteCount) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()
                || (iterator->second.info.state != Http2StreamState::Open
                    && iterator->second.info.state != Http2StreamState::HalfClosedRemote)) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 DATA cannot be sent on this stream"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::Data);
            }
            if (static_cast<std::int64_t>(byteCount) > m_impl->connectionSendWindow
                || static_cast<std::int64_t>(byteCount) > iterator->second.info.sendWindow) {
                return m_impl->RecordFailure(
                    Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 send flow-control window is exhausted"),
                    HttpErrorOrigin::Resource,
                    HttpErrorScope::Stream,
                    streamId,
                    true,
                    Http2FrameType::Data);
            }
            m_impl->connectionSendWindow -= byteCount;
            iterator->second.info.sendWindow -= byteCount;
            return {};
        }

        Result<void> Http2Session::ReceiveData(
            std::uint32_t streamId, std::uint32_t byteCount, bool endStream) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            if (m_impl->continuationStream != 0) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 DATA interrupted CONTINUATION"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::Data);
            }
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 DATA arrived on an idle stream"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::Data);
            }
            auto& info = iterator->second.info;
            if (info.state != Http2StreamState::Open
                && info.state != Http2StreamState::HalfClosedLocal) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 DATA arrived after remote stream close"),
                    HttpErrorOrigin::Protocol,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::Data);
            }
            if (static_cast<std::int64_t>(byteCount) > m_impl->connectionReceiveWindow
                || static_cast<std::int64_t>(byteCount) > info.receiveWindow) {
                return m_impl->FailConnection(
                    Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 receive flow-control window is exhausted"),
                    Http2ErrorCode::FlowControlError,
                    streamId, true, Http2FrameType::Data,
                    HttpErrorOrigin::Resource);
            }
            m_impl->connectionReceiveWindow -= byteCount;
            info.receiveWindow -= byteCount;
            if (endStream) {
                info.state = info.state == Http2StreamState::HalfClosedLocal
                    ? Http2StreamState::Closed : Http2StreamState::HalfClosedRemote;
            }
            return {};
        }

        Result<void> Http2Session::ConsumeReceivedData(
            std::uint32_t streamId, std::uint32_t byteCount) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()
                || iterator->second.info.state == Http2StreamState::Idle) {
                return m_impl->RecordFailure(
                    Status(StatusCode::NotFound, u"HTTP/2 stream was not found"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId);
            }
            auto& info = iterator->second.info;
            if (m_impl->connectionReceiveWindow + byteCount > kHttp2MaxWindowSize
                || info.receiveWindow + byteCount > kHttp2MaxWindowSize) {
                return m_impl->FailConnection(
                    Status(StatusCode::OutOfRange,
                        u"HTTP/2 receive window update overflows"),
                    Http2ErrorCode::FlowControlError,
                    streamId, false, Http2FrameType::WindowUpdate,
                    HttpErrorOrigin::Resource);
            }
            m_impl->connectionReceiveWindow += byteCount;
            info.receiveWindow += byteCount;
            return {};
        }

        Result<void> Http2Session::EndStream(std::uint32_t streamId, bool local) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()) {
                return m_impl->RecordFailure(
                    Status(StatusCode::NotFound, u"HTTP/2 stream was not found"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId);
            }
            auto& info = iterator->second.info;
            if (local) {
                if (info.state == Http2StreamState::Open) info.state = Http2StreamState::HalfClosedLocal;
                else if (info.state == Http2StreamState::HalfClosedRemote) info.state = Http2StreamState::Closed;
                else return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 local side is already closed"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId);
            } else {
                if (info.state == Http2StreamState::Open) info.state = Http2StreamState::HalfClosedRemote;
                else if (info.state == Http2StreamState::HalfClosedLocal) info.state = Http2StreamState::Closed;
                else return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 remote side is already closed"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId);
            }
            return {};
        }

        Result<void> Http2Session::ApplyWindowUpdate(
            std::uint32_t streamId, std::uint32_t increment) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            if (increment == 0 || increment > kHttp2MaxWindowSize) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 WINDOW_UPDATE increment is invalid"),
                    Http2ErrorCode::ProtocolError,
                    streamId, true, Http2FrameType::WindowUpdate);
            }
            if (streamId == 0) {
                if (m_impl->connectionSendWindow + increment > kHttp2MaxWindowSize) {
                    return m_impl->FailConnection(
                        Status(StatusCode::OutOfRange, u"HTTP/2 connection send window overflows"),
                        Http2ErrorCode::FlowControlError,
                        0, true, Http2FrameType::WindowUpdate,
                        HttpErrorOrigin::Resource);
                }
                m_impl->connectionSendWindow += increment;
                return {};
            }
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()
                || iterator->second.info.state == Http2StreamState::Closed) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 WINDOW_UPDATE targets a closed stream"),
                    HttpErrorOrigin::Protocol,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::WindowUpdate);
            }
            if (iterator->second.info.sendWindow + increment > kHttp2MaxWindowSize) {
                return m_impl->FailConnection(
                    Status(StatusCode::OutOfRange, u"HTTP/2 stream send window overflows"),
                    Http2ErrorCode::FlowControlError,
                    streamId, true, Http2FrameType::WindowUpdate,
                    HttpErrorOrigin::Resource);
            }
            iterator->second.info.sendWindow += increment;
            return {};
        }

        Result<void> Http2Session::ResetStream(
            std::uint32_t streamId, Http2ErrorCode errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            (void)errorCode;
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()
                || iterator->second.info.state == Http2StreamState::Closed) {
                return m_impl->RecordFailure(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/2 RST_STREAM targets an idle or closed stream"),
                    HttpErrorOrigin::Lifecycle,
                    HttpErrorScope::Stream,
                    streamId, true, Http2FrameType::RstStream);
            }
            iterator->second.info.state = Http2StreamState::Closed;
            iterator->second.info.headerBlockOpen = false;
            if (m_impl->continuationStream == streamId) m_impl->continuationStream = 0;
            return {};
        }

        Result<void> Http2Session::SendGoaway(
            std::uint32_t lastStreamId, Http2ErrorCode errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            (void)errorCode;
            if (lastStreamId > 0x7FFFFFFFU || lastStreamId > m_impl->localGoawayLastStream) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 GOAWAY last stream id increased"),
                    Http2ErrorCode::ProtocolError,
                    lastStreamId, true, Http2FrameType::Goaway);
            }
            m_impl->localGoawayLastStream = lastStreamId;
            m_impl->state = Http2SessionState::GoingAway;
            return {};
        }

        Result<void> Http2Session::ReceiveGoaway(
            std::uint32_t lastStreamId, Http2ErrorCode errorCode) {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            auto usable = m_impl->RequireUsable();
            if (!usable.IsOk()) return usable;
            (void)errorCode;
            if (lastStreamId > 0x7FFFFFFFU || lastStreamId > m_impl->peerGoawayLastStream) {
                return m_impl->FailConnection(
                    Status::InvalidArgument(u"HTTP/2 peer GOAWAY last stream id increased"),
                    Http2ErrorCode::ProtocolError,
                    lastStreamId, true, Http2FrameType::Goaway);
            }
            m_impl->peerGoawayLastStream = lastStreamId;
            m_impl->state = Http2SessionState::GoingAway;
            return {};
        }

        Http2SessionState Http2Session::State() const noexcept {
            return m_impl ? m_impl->state : Http2SessionState::Closed;
        }

        bool Http2Session::IsClient() const noexcept {
            return m_impl != nullptr && m_impl->client;
        }

        std::int64_t Http2Session::ConnectionSendWindow() const noexcept {
            return m_impl ? m_impl->connectionSendWindow : 0;
        }

        std::int64_t Http2Session::ConnectionReceiveWindow() const noexcept {
            return m_impl ? m_impl->connectionReceiveWindow : 0;
        }

        Result<Http2StreamInfo> Http2Session::Stream(std::uint32_t streamId) const {
            if (!m_impl) return Status::Internal(u"HTTP/2 session is moved-from");
            const auto iterator = m_impl->streams.find(streamId);
            if (iterator == m_impl->streams.end()) {
                return Status::NotFound(u"HTTP/2 stream was not found");
            }
            return iterator->second.info;
        }

        Http2SessionSnapshot Http2Session::Snapshot() const noexcept {
            if (!m_impl) return {};
            return Http2SessionSnapshot{
                m_impl->client,
                m_impl->state,
                m_impl->lastError,
                m_impl->limits,
                m_impl->peerInitialWindow,
                m_impl->peerMaxConcurrentStreams,
                m_impl->peerMaxFrameSize,
                m_impl->connectionSendWindow,
                m_impl->connectionReceiveWindow,
                m_impl->ActiveStreams(true),
                m_impl->ActiveStreams(false),
                m_impl->continuationStream,
                m_impl->settingsAckPendingCount,
                m_impl->localGoawayLastStream,
                m_impl->peerGoawayLastStream
            };
        }

        Http2ErrorCode Http2Session::LastError() const noexcept {
            return m_impl ? m_impl->lastError : Http2ErrorCode::InternalError;
        }

        HttpErrorContext Http2Session::LastHttpErrorContext() const noexcept {
            return m_impl ? m_impl->errorContext : HttpErrorContext{};
        }

        Status Http2Session::LastStatus() const {
            return m_impl ? m_impl->lastStatus
                : Status::Internal(u"HTTP/2 session is moved-from");
        }

        struct Http2StreamBodyDecoder::Impl {
            explicit Impl(std::uint32_t expectedStreamId, Http2StreamBodyLimits bodyLimits)
                : streamId(expectedStreamId), limits(bodyLimits) { }

            Result<void> Fail(const Status& failure) {
                if (status.IsOk()) status = failure;
                return status;
            }

            std::uint32_t streamId = 0;
            Http2StreamBodyLimits limits;
            std::vector<std::uint8_t> body;
            HttpBodySink* sink = nullptr;
            std::size_t receivedBodyBytes = 0;
            Status status;
            bool complete = false;
        };

        Http2StreamBodyDecoder::Http2StreamBodyDecoder(
            std::uint32_t streamId,
            Http2StreamBodyLimits limits)
            : m_impl(std::make_unique<Impl>(streamId, limits)) { }

        Http2StreamBodyDecoder::~Http2StreamBodyDecoder() = default;

        Http2StreamBodyDecoder::Http2StreamBodyDecoder(
            Http2StreamBodyDecoder&&) noexcept = default;

        Http2StreamBodyDecoder& Http2StreamBodyDecoder::operator=(
            Http2StreamBodyDecoder&&) noexcept = default;

        Result<void> Http2StreamBodyDecoder::AttachBodySink(HttpBodySink* sink) {
            if (!m_impl) return Status::Internal(u"HTTP/2 stream decoder is moved-from");
            if (sink == nullptr) return Status::InvalidArgument(u"HTTP/2 body sink is null");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->complete || !m_impl->body.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/2 body sink must be attached before DATA");
            }
            m_impl->sink = sink;
            return {};
        }

        bool Http2StreamBodyDecoder::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->sink != nullptr;
        }

        Result<void> Http2StreamBodyDecoder::Feed(const Http2Frame& frame) {
            if (!m_impl) return Status::Internal(u"HTTP/2 stream decoder is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->complete) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/2 DATA frame arrived after END_STREAM"));
            }
            if (frame.type != static_cast<std::uint8_t>(Http2FrameType::Data)) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/2 stream decoder accepts DATA frames only"));
            }
            if (frame.streamId != m_impl->streamId) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/2 DATA frame stream id mismatch"));
            }
            if (frame.length != frame.payload.size()) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/2 DATA frame length mismatch"));
            }
            if (frame.payload.size() > m_impl->limits.maxBodyBytes
                || m_impl->receivedBodyBytes > m_impl->limits.maxBodyBytes
                || frame.payload.size() > m_impl->limits.maxBodyBytes
                    - m_impl->receivedBodyBytes) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/2 stream body exceeds configured limit"));
            }

            if (m_impl->sink != nullptr) {
                if (frame.payload.size() > m_impl->sink->WritableBytes()) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 body sink cannot accept the complete DATA frame");
                }
                auto pushed = m_impl->sink->Push(frame.payload.data(), frame.payload.size());
                if (!pushed.IsOk()) return m_impl->Fail(pushed.GetStatus());
                if (pushed.Value() != frame.payload.size()) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 body sink accepted only part of the DATA frame");
                }
            } else {
                m_impl->body.insert(m_impl->body.end(), frame.payload.begin(), frame.payload.end());
            }
            m_impl->receivedBodyBytes += frame.payload.size();
            if ((frame.flags & 0x1U) != 0) {
                if (m_impl->sink != nullptr) {
                    auto closed = m_impl->sink->Close();
                    if (!closed.IsOk()) return m_impl->Fail(closed.GetStatus());
                }
                m_impl->complete = true;
            }
            return {};
        }

        Result<void> Http2StreamBodyDecoder::Finish() const {
            if (!m_impl) return Status::Internal(u"HTTP/2 stream decoder is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (!m_impl->complete) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/2 stream has not received END_STREAM");
            }
            return {};
        }

        void Http2StreamBodyDecoder::Cancel() noexcept {
            Cancel(HttpBodyCancelReason::Application);
        }

        void Http2StreamBodyDecoder::Cancel(HttpBodyCancelReason reason) noexcept {
            if (m_impl && m_impl->status.IsOk() && !m_impl->complete) {
                if (m_impl->sink != nullptr) m_impl->sink->Cancel(reason);
                m_impl->status = Status(StatusCode::Cancelled,
                    u"HTTP/2 stream body was cancelled");
            }
        }

        bool Http2StreamBodyDecoder::IsComplete() const noexcept {
            return m_impl != nullptr && m_impl->complete && m_impl->status.IsOk();
        }

        bool Http2StreamBodyDecoder::IsCancelled() const noexcept {
            return m_impl != nullptr && m_impl->status.Code() == StatusCode::Cancelled;
        }

        const std::vector<std::uint8_t>& Http2StreamBodyDecoder::Body() const noexcept {
            static const std::vector<std::uint8_t> empty;
            return m_impl ? m_impl->body : empty;
        }

        void Http2StreamBodyDecoder::Reset() noexcept {
            if (!m_impl) return;
            if (m_impl->sink != nullptr) m_impl->sink->Reset();
            m_impl->body.clear();
            m_impl->receivedBodyBytes = 0;
            m_impl->status = Status::OkStatus();
            m_impl->complete = false;
        }

        const char* Http2FrameTypeName(std::uint8_t type) noexcept {
            switch (static_cast<Http2FrameType>(type)) {
            case Http2FrameType::Data: return "DATA";
            case Http2FrameType::Headers: return "HEADERS";
            case Http2FrameType::Priority: return "PRIORITY";
            case Http2FrameType::RstStream: return "RST_STREAM";
            case Http2FrameType::Settings: return "SETTINGS";
            case Http2FrameType::PushPromise: return "PUSH_PROMISE";
            case Http2FrameType::Ping: return "PING";
            case Http2FrameType::Goaway: return "GOAWAY";
            case Http2FrameType::WindowUpdate: return "WINDOW_UPDATE";
            case Http2FrameType::Continuation: return "CONTINUATION";
            }

            return "UNKNOWN";
        }
    }
}
