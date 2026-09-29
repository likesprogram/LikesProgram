#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Http/HttpObservability.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LikesProgram {
    namespace Http {
        struct HttpHeader {
            std::string name;
            std::string value;
        };

        struct Http1MessageLimits {
            std::size_t maxHeaderBytes      = 64 * 1024;
            std::size_t maxBodyBytes        = 8 * 1024 * 1024;
            std::size_t maxTrailerBytes     = 16 * 1024;
            std::size_t maxChunkLineBytes   = 8 * 1024;
        };

        struct HttpRequest {
            std::string method;
            std::string target;
            std::string version = "HTTP/1.1";
            std::vector<HttpHeader> headers;
            std::vector<std::uint8_t> body;
            std::vector<HttpHeader> trailers;
        };

        struct HttpResponse {
            std::string version = "HTTP/1.1";
            int statusCode = 200;
            std::string reason = "OK";
            std::vector<HttpHeader> headers;
            std::vector<std::uint8_t> body;
            std::vector<HttpHeader> trailers;
        };

        // 响应所在请求的语义上下文；默认表示普通响应。
        struct Http1ResponseContext {
            bool requestWasHead = false;
            bool requestWasConnect = false;
            bool requestWantsUpgrade = false;
        };

        enum class Http1MessageKind : std::uint8_t {
            Request,
            Response
        };

        enum class Http1ConnectionState : std::uint8_t {
            Open,
            Closing,
            Upgraded,
            Tunnel,
            Closed
        };

        enum class Http1RequestTargetForm : std::uint8_t {
            Origin,
            Absolute,
            Authority,
            Asterisk
        };

        struct Http1ConnectionDecision {
            bool keepAlive = true;
            bool closeDelimited = false;
            bool upgrade = false;
            bool tunnel = false;
        };

        struct Http1ConnectionSnapshot {
            Http1MessageKind kind = Http1MessageKind::Request;
            Http1ConnectionState state = Http1ConnectionState::Closed;
            Http1MessageLimits limits{};
            std::size_t bufferedBytes = 0;
            bool finished = false;
        };

        class Http1ChunkedDecoder {
        public:
            LIKESPROGRAM_HTTP_API explicit Http1ChunkedDecoder(Http1MessageLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http1ChunkedDecoder();

            LIKESPROGRAM_HTTP_API Http1ChunkedDecoder(Http1ChunkedDecoder&&) noexcept;
            LIKESPROGRAM_HTTP_API Http1ChunkedDecoder& operator=(Http1ChunkedDecoder&&) noexcept;
            Http1ChunkedDecoder(const Http1ChunkedDecoder&) = delete;
            Http1ChunkedDecoder& operator=(const Http1ChunkedDecoder&) = delete;

            // The sink is non-owning. On backpressure, Feed retains pending input;
            // drain/resume the sink, then call Feed with an empty view to retry.
            LIKESPROGRAM_HTTP_API Result<void> Feed(std::string_view bytes);
            LIKESPROGRAM_HTTP_API Result<void> AttachBodySink(HttpBodySink* sink);
            LIKESPROGRAM_HTTP_API bool HasBodySink() const noexcept;
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API void Cancel() noexcept;
            LIKESPROGRAM_HTTP_API void Cancel(HttpBodyCancelReason reason) noexcept;
            LIKESPROGRAM_HTTP_API bool IsComplete() const noexcept;
            LIKESPROGRAM_HTTP_API bool IsCancelled() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<std::uint8_t>& Body() const noexcept;
            LIKESPROGRAM_HTTP_API const std::vector<HttpHeader>& Trailers() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        LIKESPROGRAM_HTTP_API Result<HttpRequest> ParseHttp1Request(std::string_view message);
        LIKESPROGRAM_HTTP_API Result<HttpRequest> ParseHttp1Request(std::string_view message, const Http1MessageLimits& limits);

        LIKESPROGRAM_HTTP_API Result<HttpResponse> ParseHttp1Response(std::string_view message);
        LIKESPROGRAM_HTTP_API Result<HttpResponse> ParseHttp1Response(std::string_view message, const Http1MessageLimits& limits);
        LIKESPROGRAM_HTTP_API Result<HttpResponse> ParseHttp1Response(std::string_view message, const Http1MessageLimits& limits, const Http1ResponseContext& context);

        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttp1Request(const HttpRequest& request);
        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttp1Request(const HttpRequest& request, const Http1MessageLimits& limits);

        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttp1Response(const HttpResponse& response);
        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttp1Response(const HttpResponse& response, const Http1MessageLimits& limits);
        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttp1Response(const HttpResponse& response, const Http1MessageLimits& limits, const Http1ResponseContext& context);

        LIKESPROGRAM_HTTP_API Result<Http1RequestTargetForm> ClassifyHttp1RequestTarget(std::string_view method, std::string_view target);

        LIKESPROGRAM_HTTP_API Result<Http1ConnectionDecision> EvaluateHttp1Connection(const HttpRequest& request, const HttpResponse& response);

        // Sans-I/O HTTP/1 pipeline reader. It never owns a socket or transport.
        class Http1Connection {
        public:
            LIKESPROGRAM_HTTP_API explicit Http1Connection(Http1MessageKind kind, Http1MessageLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http1Connection();

            Http1Connection(const Http1Connection&) = delete;
            Http1Connection& operator=(const Http1Connection&) = delete;
            LIKESPROGRAM_HTTP_API Http1Connection(Http1Connection&&) noexcept;
            LIKESPROGRAM_HTTP_API Http1Connection& operator=(Http1Connection&&) noexcept;

            LIKESPROGRAM_HTTP_API Result<void> Feed(std::string_view bytes);
            LIKESPROGRAM_HTTP_API Result<std::optional<HttpRequest>> NextRequest();
            LIKESPROGRAM_HTTP_API Result<std::optional<HttpResponse>> NextResponse(const Http1ResponseContext& context = {});
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API Result<void> Drain();
            LIKESPROGRAM_HTTP_API Http1ConnectionState State() const noexcept;
            LIKESPROGRAM_HTTP_API std::size_t BufferedBytes() const noexcept;
            LIKESPROGRAM_HTTP_API Http1ConnectionSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API HttpErrorContext LastHttpErrorContext() const noexcept;
            LIKESPROGRAM_HTTP_API Status LastError() const;
            LIKESPROGRAM_HTTP_API std::string TakeBufferedBytes();
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        LIKESPROGRAM_HTTP_API std::string HttpHeaderValue(const std::vector<HttpHeader>& headers, std::string_view name);
    }
}
