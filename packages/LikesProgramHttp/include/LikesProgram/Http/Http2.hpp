#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Http/HttpHeaderBlock.hpp>
#include <LikesProgram/Http/HttpObservability.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace LikesProgram {
    namespace Http {
        // HTTP/2 固定连接前言，客户端连接建立后必须先发送这段字节。
        inline constexpr std::string_view kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

        // HTTP/2 标准帧类型值；未知类型仍可通过原始 uint8_t 透传。
        enum class Http2FrameType : std::uint8_t {
            Data         = 0x0,
            Headers      = 0x1,
            Priority     = 0x2,
            RstStream    = 0x3,
            Settings     = 0x4,
            PushPromise  = 0x5,
            Ping         = 0x6,
            Goaway       = 0x7,
            WindowUpdate = 0x8,
            Continuation = 0x9
        };

        // HTTP/2 二进制帧，表示 9 字节帧头和紧随其后的 payload。
        struct Http2Frame {
            std::uint32_t length = 0;              // payload 长度，解析时来自 24-bit 字段
            std::uint8_t type = 0;                 // 原始帧类型，允许未知扩展类型
            std::uint8_t flags = 0;                // 原始 flags 字节，由上层语义解释
            std::uint32_t streamId = 0;            // 31-bit stream id，最高保留位已清除
            std::vector<std::uint8_t> payload;     // 帧负载，长度必须等于 length
        };

        // SETTINGS payload 的一个 16-bit identifier / 32-bit value 条目。
        enum class Http2SettingId : std::uint16_t {
            HeaderTableSize      = 0x1,
            EnablePush           = 0x2,
            MaxConcurrentStreams = 0x3,
            InitialWindowSize    = 0x4,
            MaxFrameSize         = 0x5,
            MaxHeaderListSize    = 0x6
        };

        struct Http2Setting {
            std::uint16_t id = 0;
            std::uint32_t value = 0;
        };

        // 编解码单个 SETTINGS payload；未知 id 按 RFC 透传，单帧重复 id 拒绝。
        LIKESPROGRAM_HTTP_API Result<std::vector<Http2Setting>> ParseHttp2Settings(const std::vector<std::uint8_t>& payload);
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp2Settings(const std::vector<Http2Setting>& settings);

        // HTTP/2 DATA 流正文的有界累积策略；实现不持有 socket 或连接状态。
        struct Http2StreamBodyLimits {
            std::size_t maxBodyBytes = 8 * 1024 * 1024;
        };

        // 按单个 stream 增量接收 HTTP/2 DATA 帧，支持独立并发实例。
        class Http2StreamBodyDecoder {
        public:
            LIKESPROGRAM_HTTP_API explicit Http2StreamBodyDecoder(std::uint32_t streamId, Http2StreamBodyLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http2StreamBodyDecoder();

            LIKESPROGRAM_HTTP_API Http2StreamBodyDecoder(Http2StreamBodyDecoder&&) noexcept;
            LIKESPROGRAM_HTTP_API Http2StreamBodyDecoder& operator=(Http2StreamBodyDecoder&&) noexcept;
            Http2StreamBodyDecoder(const Http2StreamBodyDecoder&) = delete;
            Http2StreamBodyDecoder& operator=(const Http2StreamBodyDecoder&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http2Frame& frame);
            // The sink is non-owning; Feed rejects a frame until the sink has room for it.
            LIKESPROGRAM_HTTP_API Result<void> AttachBodySink(HttpBodySink* sink);
            LIKESPROGRAM_HTTP_API bool HasBodySink() const noexcept;
            LIKESPROGRAM_HTTP_API Result<void> Finish() const;
            LIKESPROGRAM_HTTP_API void Cancel() noexcept;
            LIKESPROGRAM_HTTP_API void Cancel(HttpBodyCancelReason reason) noexcept;
            LIKESPROGRAM_HTTP_API bool IsComplete() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsCancelled() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::uint8_t>& Body() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        enum class Http2SessionState : std::uint8_t {
            Open,
            GoingAway,
            Closed
        };

        enum class Http2StreamState : std::uint8_t {
            Idle,
            Open,
            HalfClosedLocal,
            HalfClosedRemote,
            Closed
        };

        enum class Http2ErrorCode : std::uint32_t {
            NoError          = 0x0,
            ProtocolError    = 0x1,
            InternalError    = 0x2,
            FlowControlError = 0x3,
            StreamClosed     = 0x5,
            RefusedStream    = 0x7,
            Cancel           = 0x8,
            CompressionError = 0x9,
            EnhanceYourCalm  = 0xB
        };

        enum class Http2BodyCancellationActionKind : std::uint8_t {
            ResetStream,
            AlreadyHandled
        };

        struct Http2BodyCancellationAction {
            Http2BodyCancellationActionKind kind = Http2BodyCancellationActionKind::ResetStream;
            Http2ErrorCode errorCode = Http2ErrorCode::Cancel;
        };

        LIKESPROGRAM_HTTP_API Result<Http2BodyCancellationAction> MapHttp2BodyCancellation(HttpBodyCancelReason reason);

        struct Http2SessionLimits {
            std::uint32_t initialWindowSize = 65535;
            std::uint32_t maxConcurrentStreams = 100;
            std::uint32_t maxFrameSize = 16384;
        };

        // Read-only connection/stream resource state exposed for adapters and
        // diagnostics; it does not own transport, HPACK, or QPACK state.
        struct Http2SessionSnapshot {
            bool client = false;
            Http2SessionState state = Http2SessionState::Closed;
            Http2ErrorCode lastError = Http2ErrorCode::InternalError;
            Http2SessionLimits localLimits{};
            std::uint32_t peerInitialWindow = 0;
            std::uint32_t peerMaxConcurrentStreams = 0;
            std::uint32_t peerMaxFrameSize = 0;
            std::int64_t connectionSendWindow = 0;
            std::int64_t connectionReceiveWindow = 0;
            std::size_t activeLocalStreams = 0;
            std::size_t activeRemoteStreams = 0;
            std::uint32_t continuationStream = 0;
            std::uint32_t settingsAckPending = 0;
            std::uint32_t localGoawayLastStream = 0;
            std::uint32_t peerGoawayLastStream = 0;
        };

        struct Http2StreamInfo {
            std::uint32_t streamId = 0;
            bool localInitiated = false;
            Http2StreamState state = Http2StreamState::Idle;
            std::int64_t sendWindow = 0;
            std::int64_t receiveWindow = 0;
            bool headersReceived = false;
            bool headerBlockOpen = false;
        };

        // 无 socket/QUIC/HPACK 所有权的 HTTP/2 connection + stream state machine。
        class Http2Session {
        public:
            LIKESPROGRAM_HTTP_API explicit Http2Session(bool client, Http2SessionLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http2Session();

            LIKESPROGRAM_HTTP_API Http2Session(Http2Session&&) noexcept;
            LIKESPROGRAM_HTTP_API Http2Session& operator=(Http2Session&&) noexcept;
            Http2Session(const Http2Session&) = delete;
            Http2Session& operator=(const Http2Session&) = delete;

            // 接收 peer SETTINGS；成功后必须由 AcknowledgeSettings 清除 ACK pending。
            LIKESPROGRAM_HTTP_API Result<void> ApplySettings(const std::vector<Http2Setting>& settings);
            LIKESPROGRAM_HTTP_API Result<void> AcknowledgeSettings();
            LIKESPROGRAM_HTTP_API bool SettingsAckPending() const noexcept;

            // 本地创建 stream，或接收 peer 的首个 HEADERS/CONTINUATION 事件。
            LIKESPROGRAM_HTTP_API Result<void> OpenLocalStream(std::uint32_t streamId);
            LIKESPROGRAM_HTTP_API Result<void> ReceiveHeaders(std::uint32_t streamId, bool endStream, bool endHeaders);
            LIKESPROGRAM_HTTP_API Result<void> ContinueHeaders(std::uint32_t streamId, bool endHeaders);

            // 事件 API 只接收 DATA 字节数；实际帧编解码由调用方负责。
            LIKESPROGRAM_HTTP_API Result<void> SendData(std::uint32_t streamId, std::uint32_t byteCount);
            LIKESPROGRAM_HTTP_API Result<void> ReceiveData(std::uint32_t streamId, std::uint32_t byteCount, bool endStream);
            LIKESPROGRAM_HTTP_API Result<void> ConsumeReceivedData(std::uint32_t streamId, std::uint32_t byteCount);
            LIKESPROGRAM_HTTP_API Result<void> EndStream(std::uint32_t streamId, bool local);
            LIKESPROGRAM_HTTP_API Result<void> ApplyWindowUpdate(std::uint32_t streamId, std::uint32_t increment);

            LIKESPROGRAM_HTTP_API Result<void> ResetStream(std::uint32_t streamId, Http2ErrorCode errorCode);
            LIKESPROGRAM_HTTP_API Result<void> SendGoaway(std::uint32_t lastStreamId, Http2ErrorCode errorCode = Http2ErrorCode::NoError);
            LIKESPROGRAM_HTTP_API Result<void> ReceiveGoaway(std::uint32_t lastStreamId, Http2ErrorCode errorCode = Http2ErrorCode::NoError);

            LIKESPROGRAM_HTTP_API Http2SessionState State() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsClient() const noexcept;
            LIKESPROGRAM_HTTP_API std::int64_t ConnectionSendWindow() const noexcept;
            LIKESPROGRAM_HTTP_API std::int64_t ConnectionReceiveWindow() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http2StreamInfo> Stream(std::uint32_t streamId) const;
            LIKESPROGRAM_HTTP_API Http2SessionSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Http2ErrorCode LastError() const noexcept;
            LIKESPROGRAM_HTTP_API HttpErrorContext LastHttpErrorContext() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastStatus() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        // 判断输入是否完整匹配 HTTP/2 连接前言。
        LIKESPROGRAM_HTTP_API bool IsHttp2ConnectionPreface(const std::uint8_t* data, std::size_t size) noexcept;

        // 返回 HTTP/2 连接前言字节序列。
        LIKESPROGRAM_HTTP_API std::vector<std::uint8_t> BuildHttp2ConnectionPreface();

        // 解析单个完整 HTTP/2 帧；输入包含额外字节时返回错误。
        LIKESPROGRAM_HTTP_API Result<Http2Frame> ParseHttp2Frame(const std::uint8_t* data, std::size_t size);

        // 组装单个 HTTP/2 帧，payload 长度超过 24-bit 时返回错误。
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp2Frame(const Http2Frame& frame);

        // 返回标准帧类型名称，未知类型返回 "UNKNOWN"。
        LIKESPROGRAM_HTTP_API const char* Http2FrameTypeName(std::uint8_t type) noexcept;

        // 从字节容器解析单个完整 HTTP/2 帧。
        inline Result<Http2Frame> ParseHttp2Frame(const std::vector<std::uint8_t>& bytes) { return ParseHttp2Frame(bytes.data(), bytes.size()); }
    }
}
