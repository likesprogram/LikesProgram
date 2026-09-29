#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Http/HttpHeaderBlock.hpp>
#include <LikesProgram/Http/Http3Qpack.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Http {
        // HTTP/3 标准帧类型；未知类型仍可通过原始值透传。
        enum class Http3FrameType : std::uint64_t {
            Data         = 0x0,
            Headers      = 0x1,
            CancelPush   = 0x3,
            Settings     = 0x4,
            PushPromise  = 0x5,
            Goaway       = 0x7,
            MaxPushId    = 0xD,
            WebTransport = 0x41
        };

        // HTTP/3 帧，表示类型变长整数、长度变长整数和原始负载。
        struct Http3Frame {
            std::uint64_t type = 0;                 // 帧类型，可保留未知扩展值
            std::vector<std::uint8_t> payload;      // 帧负载，不接触 QUIC stream
        };

        // HTTP/3 DATA 流正文的有界累积策略；stream-id 属于 QUIC stream 而非 frame。
        struct Http3StreamBodyLimits {
            std::size_t maxBodyBytes         = 8 * 1024 * 1024;
            std::size_t maxHeaderBlockBytes  = 64 * 1024;
            std::size_t maxTrailerBlockBytes = 64 * 1024;
            std::size_t maxUnknownFrames     = 64;
        };

        enum class Http3ErrorCode : std::uint64_t {
            NoError              = 0x100,
            GeneralProtocolError = 0x101,
            InternalError        = 0x102,
            StreamCreationError  = 0x103,
            FrameUnexpected      = 0x105,
            ClosedCriticalStream = 0x10A,
            RequestCancelled     = 0x10C,
            MessageError         = 0x10E
        };

        enum class Http3RequestStreamState : std::uint8_t {
            AwaitingHeaders,
            Open,
            Trailers,
            Complete,
            Failed
        };

        struct Http3RequestStreamSnapshot {
            Http3RequestStreamState state = Http3RequestStreamState::AwaitingHeaders;
            std::uint64_t streamId = 0;
            std::size_t headerBlockCount = 0;
            std::size_t trailerBlockCount = 0;
            std::size_t bodyBytes = 0;
            std::size_t unknownFrameCount = 0;
            Http3ErrorCode errorCode = Http3ErrorCode::NoError;
        };

        // Request-stream failures are mapped to non-owning QUIC actions. The
        // caller applies each direction only while that stream direction is open.
        struct Http3RequestStreamQuicActions {
            std::uint64_t quicErrorCode = 0;
            bool resetStream = false;
            bool stopSending = false;
        };

        LIKESPROGRAM_HTTP_API Result<Http3RequestStreamQuicActions> MapHttp3RequestStreamError(Http3ErrorCode errorCode);

        // Converts a failed request-stream QPACK field-section decode status
        // to the existing caller-owned connection-close action.
        LIKESPROGRAM_HTTP_API Result<Http3QpackStreamQuicActions> MapHttp3RequestStreamQpackFailure(const Status& failure);

        // 接收侧 request stream 状态边界；QPACK block 仅按 opaque bytes 保存。
        class Http3RequestStream {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3RequestStream(std::uint64_t streamId, Http3StreamBodyLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3RequestStream();

            LIKESPROGRAM_HTTP_API Http3RequestStream(Http3RequestStream&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStream& operator=(Http3RequestStream&&) noexcept;
            Http3RequestStream(const Http3RequestStream&) = delete;
            Http3RequestStream& operator=(const Http3RequestStream&) = delete;

            // 每次调用交接一个已分帧的 QUIC stream frame；endStream 表示 QUIC FIN。
            LIKESPROGRAM_HTTP_API Result<void> Feed(const Http3Frame& frame, bool endStream = false);
            // Marks the QUIC stream FIN when it arrives without another
            // complete HTTP/3 frame in the same transport chunk.
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            // The sink is non-owning and must attach before any request input.
            // With a sink, DATA is handed off without retaining Body bytes.
            LIKESPROGRAM_HTTP_API Result<void> AttachBodySink(HttpBodySink* sink);
            LIKESPROGRAM_HTTP_API bool HasBodySink() const noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStreamState State() const noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::vector<std::uint8_t>>& HeaderBlocks() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::vector<std::uint8_t>>& TrailerBlocks() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::uint8_t>& Body() const noexcept;
            // Decode one caller-owned QPACK table against the stored initial
            // request header block and apply HTTP/3 request validation.
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeRequestHeaders(const Http3QpackDynamicTable& dynamicTable) const;
            // Applies the caller-owned QPACK encoded-block limit before the
            // same parse/decode path. Validation is stateless.
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeRequestHeaders(const Http3QpackDynamicTable& dynamicTable, const Http3QpackResourceBudget& resourceBudget) const;
            // Decode the stored trailer block with trailer-only validation.
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeTrailers(const Http3QpackDynamicTable& dynamicTable) const;
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeTrailers(const Http3QpackDynamicTable& dynamicTable, const Http3QpackResourceBudget& resourceBudget) const;
            // Register the stored initial HEADERS Required Insert Count with
            // a caller-owned QPACK section tracker.
            LIKESPROGRAM_HTTP_API Result<void> TrackRequestHeaderSection(Http3QpackSectionTracker& sectionTracker, const Http3QpackDynamicTable& dynamicTable) const;
            // Register the stored trailer HEADERS section in stream order.
            LIKESPROGRAM_HTTP_API Result<void> TrackTrailerSection(Http3QpackSectionTracker& sectionTracker, const Http3QpackDynamicTable& dynamicTable) const;
            LIKESPROGRAM_HTTP_API Http3ErrorCode ErrorCode() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        // 按 QUIC stream 增量接收 HTTP/3 DATA 帧，结束标记由传输层显式提供。
        struct Http3RequestStreamWireLimits {
            std::size_t maxPendingBytes = 64 * 1024;
            std::size_t maxFramePayloadBytes = 16 * 1024 * 1024;
        };

        struct Http3RequestStreamEncodeLimits {
            std::size_t maxHeaderBlockBytes = 64 * 1024;
            std::size_t maxBodyBytes = 8 * 1024 * 1024;
            std::size_t maxDataFrames = 1024;
            std::size_t maxFramePayloadBytes = 16 * 1024 * 1024;
        };

        // Composes opaque QPACK HEADERS bytes and DATA chunks. QPACK decoding,
        // FIN and QUIC stream ownership remain caller responsibilities.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3RequestStreamBytes(
            const std::vector<std::uint8_t>& qpackHeaders, const std::vector<std::vector<std::uint8_t>>& dataChunks, Http3RequestStreamEncodeLimits limits = {}
        );

        // Composes an initial opaque QPACK HEADERS block, DATA chunks and an
        // optional trailing opaque HEADERS block in response order.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3ResponseStreamBytes(
            const std::vector<std::uint8_t>& initialQpackHeaders, const std::vector<std::vector<std::uint8_t>>& dataChunks,
            const std::optional<std::vector<std::uint8_t>>& trailingQpackHeaders = std::nullopt, Http3RequestStreamEncodeLimits limits = {}
        );

        // Reassembles arbitrarily chunked QUIC stream bytes into HTTP/3
        // frames without owning the QUIC stream, socket, TLS or timer.
        class Http3RequestStreamWireDecoder {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3RequestStreamWireDecoder(std::uint64_t streamId, Http3StreamBodyLimits bodyLimits = {}, Http3RequestStreamWireLimits wireLimits = {});
            LIKESPROGRAM_HTTP_API ~Http3RequestStreamWireDecoder();

            LIKESPROGRAM_HTTP_API Http3RequestStreamWireDecoder(Http3RequestStreamWireDecoder&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStreamWireDecoder& operator=(Http3RequestStreamWireDecoder&&) noexcept;
            Http3RequestStreamWireDecoder(const Http3RequestStreamWireDecoder&) = delete;
            Http3RequestStreamWireDecoder& operator=(const Http3RequestStreamWireDecoder&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::uint8_t* data, std::size_t size, bool endStream = false);
            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::vector<std::uint8_t>& bytes, bool endStream = false);
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API Result<void> AttachBodySink(HttpBodySink* sink);
            LIKESPROGRAM_HTTP_API bool HasBodySink() const noexcept;
            // Retries one complete DATA frame retained after sink backpressure.
            LIKESPROGRAM_HTTP_API Result<void> RetryPendingBody();
            LIKESPROGRAM_HTTP_API bool HasPendingBodyRetry() const noexcept;
            // Reports DATA payload bytes retained for the explicit retry.
            LIKESPROGRAM_HTTP_API std::size_t PendingBodyBytes() const noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStreamWireLimits WireLimits() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t PendingBytes() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsComplete() const noexcept;
            LIKESPROGRAM_HTTP_API Http3RequestStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            // Maps a decoder or request-stream failure to caller-owned H3
            // RESET_STREAM/STOP_SENDING intent without emitting transport I/O.
            LIKESPROGRAM_HTTP_API Result<Http3RequestStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API const std::vector<std::vector<std::uint8_t>>& HeaderBlocks() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::vector<std::uint8_t>>& TrailerBlocks() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::uint8_t>& Body() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        inline Result<void> Http3RequestStreamWireDecoder::Feed(
            const std::vector<std::uint8_t>& bytes,
            bool endStream) {
            return Feed(bytes.data(), bytes.size(), endStream);
        }

        class Http3StreamBodyDecoder {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3StreamBodyDecoder(std::uint64_t streamId, Http3StreamBodyLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3StreamBodyDecoder();

            LIKESPROGRAM_HTTP_API Http3StreamBodyDecoder(Http3StreamBodyDecoder&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3StreamBodyDecoder& operator=(Http3StreamBodyDecoder&&) noexcept;
            Http3StreamBodyDecoder(const Http3StreamBodyDecoder&) = delete;
            Http3StreamBodyDecoder& operator=(const Http3StreamBodyDecoder&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(std::uint64_t streamId, const Http3Frame& frame, bool endStream);
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

        // 解析一个完整 QUIC 变长整数，返回值和占用字节数。
        LIKESPROGRAM_HTTP_API Result<std::pair<std::uint64_t, std::size_t>> ParseHttp3VarInt(const std::uint8_t* data, std::size_t size);

        // 组装一个 62-bit QUIC 变长整数。
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3VarInt(std::uint64_t value);

        // 解析一个完整 HTTP/3 帧；输入额外字节或长度不一致时返回错误。
        LIKESPROGRAM_HTTP_API Result<Http3Frame> ParseHttp3Frame(const std::uint8_t* data, std::size_t size);

        // 组装一个完整 HTTP/3 帧；不创建 QUIC 连接或 socket。
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3Frame(const Http3Frame& frame);

        // 返回标准 HTTP/3 帧类型名称，未知类型返回 UNKNOWN。
        LIKESPROGRAM_HTTP_API const char* Http3FrameTypeName(std::uint64_t type) noexcept;

        // 从字节容器解析一个完整 HTTP/3 帧。
        inline Result<Http3Frame> ParseHttp3Frame(const std::vector<std::uint8_t>& bytes) { return ParseHttp3Frame(bytes.data(), bytes.size()); }
    }
}
