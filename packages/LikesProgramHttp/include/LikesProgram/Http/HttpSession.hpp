#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpObservability.hpp>
#include <LikesProgram/Http/Http1.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace LikesProgram {
    namespace Http {
        // Session 使用的协议选择；传输实现由应用侧注入。
        // Bounded caller-policy input for replaying one request on a TCP
        // fallback after a transport-level failure. The Session still owns no
        // connection, socket, TLS provider, timer, or retry scheduler.
        struct HttpRequestReplayPolicy {
            std::size_t maxAttempts = 2;
            bool retryUnavailable = true;
            bool retryDeadlineExceeded = false;
            bool allowUnsafeMethodsWithIdempotencyKey = false;
        };

        LIKESPROGRAM_HTTP_API bool IsHttpRequestReplayable(const HttpRequest& request, const HttpRequestReplayPolicy& policy = {}) noexcept;

        struct HttpNegotiatedProtocol {
            HttpVersion version = HttpVersion::Http1; // ALPN/adapter 确认的 HTTP 版本
            const char* alpn = ""; // 协商后的 ALPN 文本，调用方保持其生命周期
            bool secure = false; // 是否由 TLS/QUIC 安全层保护
            bool datagram = false; // 是否由 UDP/QUIC datagram 承载
            bool fallback = false; // 是否从更高版本回退到当前 TCP 版本
        };

        // 把 ALPN 文本映射为 HTTP 版本；未知文本返回 false，不猜测协议。
        LIKESPROGRAM_HTTP_API bool TryMapHttpAlpn(std::string_view alpn, HttpVersion& version) noexcept;

        // ALPN 鏂囨湰涓庝紶杈撳睘鎬х粍鍚堟垚 session context；ALPN 鏂囨湰鐢遍€傞厤鍣ㄩ潪鎷ユ湁銆?
        LIKESPROGRAM_HTTP_API bool TryBuildHttpNegotiatedProtocol(const char* alpn, bool secure, bool datagram, bool fallback, HttpNegotiatedProtocol& negotiated) noexcept;

        // 用户提供的请求传输适配器，不拥有 socket、线程或第三方网络对象。
        class LIKESPROGRAM_HTTP_API HttpTransport {
        public:
            virtual ~HttpTransport() = default;

            // 发送一个已解析请求并返回响应；实现方自行组合传输实现。
            virtual Result<HttpResponse> Exchange(const HttpRequest& request, HttpVersion version) = 0;

            // 使用真实协商上下文发起请求；默认回退到旧版本参数入口。
            virtual Result<HttpResponse> Exchange(const HttpRequest& request, const HttpNegotiatedProtocol& negotiated) {
                return Exchange(request, negotiated.version);
            }
        };

        // 用户提供的服务端请求处理器，不负责监听、读写或连接生命周期。
        class LIKESPROGRAM_HTTP_API HttpRequestHandler {
        public:
            virtual ~HttpRequestHandler() = default;

            // 处理一个请求并生成响应。
            virtual Result<HttpResponse> Handle(const HttpRequest& request) = 0;
        };

        // 轻量 HTTP 会话；Transport 与 Handler 均为非拥有指针，默认不连接网络。
        class LIKESPROGRAM_HTTP_API HttpSession {
        public:
            // 创建可选的用户传输和请求处理器绑定。
            explicit HttpSession(HttpTransport* transport = nullptr, HttpRequestHandler* handler = nullptr);

            // 释放会话内部状态，不释放用户注入对象。
            ~HttpSession();

            HttpSession(const HttpSession&) = delete;
            HttpSession& operator=(const HttpSession&) = delete;

            // 设置非拥有的请求传输适配器。
            void SetTransport(HttpTransport* transport) noexcept;

            // 设置非拥有的服务端请求处理器。
            void SetHandler(HttpRequestHandler* handler) noexcept;

            // 返回当前协议选择。
            HttpVersion Version() const noexcept;

            // 更新后续请求使用的协议选择。
            void SetVersion(HttpVersion version) noexcept;

            // 设置由 ALPN/QUIC adapter 确认的连接协议上下文。
            void SetNegotiatedProtocol(const HttpNegotiatedProtocol& negotiated) noexcept;

            // 浠ュ栭儴 ALPN/transport 鐜伴亾鏇存柊涓嬫枃锛涘け璐ユ椂淇濇寔鍘熸湁涓嬫枃涓嶅彉銆?
            bool SetNegotiatedAlpn(const char* alpn, bool secure, bool datagram, bool fallback = false) noexcept;

            // Caller-owned request gate shared by all negotiated versions.
            void SetDeadline(LikesProgram::Time::Deadline deadline) noexcept;
            bool HasDeadline() const noexcept;
            bool DeadlineExpired() const noexcept;
            void Cancel() noexcept;
            void ResetCancellation() noexcept;
            bool IsCancelled() const noexcept;

            // 返回当前连接协议上下文快照。
            HttpNegotiatedProtocol NegotiatedProtocol() const noexcept;

            // 通过注入的 Transport 发起请求；未配置或回调抛出时返回失败状态。
            Result<HttpResponse> Send(const HttpRequest& request);

            // Replays the same request once on an explicit HTTP/1.1 or HTTP/2
            // fallback after an allowed transport failure. No transport,
            // connection, TLS, timer, or scheduler is owned by the Session.
            Result<HttpResponse> SendWithFallback(const HttpRequest& request, HttpVersion fallbackVersion, const HttpRequestReplayPolicy& policy = {});

            // 通过注入的 Handler 处理请求；未配置或回调抛出时返回失败状态。
            Result<HttpResponse> Handle(const HttpRequest& request);

        private:
            struct HttpSessionImpl;
            HttpSessionImpl* m_impl = nullptr; // 会话状态与非拥有适配器指针
        };

        // 返回协议版本稳定名称。
        LIKESPROGRAM_HTTP_API const char* HttpVersionName(HttpVersion version) noexcept;
    }
}
