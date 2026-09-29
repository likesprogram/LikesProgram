#include <LikesProgram/Http/HttpSession.hpp>

#include <string_view>

namespace LikesProgram {
    namespace Http {
        namespace {
            bool IsIdempotentMethod(std::string_view method) noexcept {
                return method == "GET" || method == "HEAD" || method == "PUT"
                    || method == "DELETE" || method == "OPTIONS"
                    || method == "TRACE";
            }

            bool IsIdempotencyKeyName(std::string_view name) noexcept {
                if (name.size() != std::string_view("idempotency-key").size()) {
                    return false;
                }
                for (std::size_t index = 0; index < name.size(); ++index) {
                    char actual = name[index];
                    if (actual >= 'A' && actual <= 'Z') {
                        actual = static_cast<char>(actual - 'A' + 'a');
                    }
                    if (actual != std::string_view("idempotency-key")[index]) {
                        return false;
                    }
                }
                return true;
            }

            bool IsRetryableStatus(
                StatusCode code,
                const HttpRequestReplayPolicy& policy) noexcept {
                if (code == StatusCode::Unavailable) {
                    return policy.retryUnavailable;
                }
                if (code == StatusCode::DeadlineExceeded) {
                    return policy.retryDeadlineExceeded;
                }
                return false;
            }

            const char* FallbackAlpn(HttpVersion version) noexcept {
                switch (version) {
                case HttpVersion::Http1: return "http/1.1";
                case HttpVersion::Http2: return "h2";
                case HttpVersion::Http3: return nullptr;
                }
                return nullptr;
            }
        }

        struct HttpSession::HttpSessionImpl {
            LikesProgram::Time::Deadline deadline =
                LikesProgram::Time::Deadline::Infinite();
            bool cancelled = false;
            HttpTransport* transport = nullptr; // 用户拥有的非拥有传输适配器
            HttpRequestHandler* handler = nullptr; // 用户拥有的非拥有请求处理器
            HttpVersion version = HttpVersion::Http1; // 默认协议选择
            HttpNegotiatedProtocol negotiated{}; // 真实 ALPN/adapter 协议上下文
        };

        bool TryMapHttpAlpn(
            std::string_view alpn,
            HttpVersion& version) noexcept {
            if (alpn == "http/1.1") {
                version = HttpVersion::Http1;
                return true;
            }
            if (alpn == "h2") {
                version = HttpVersion::Http2;
                return true;
            }
            if (alpn == "h3" || alpn.starts_with("h3-")) {
                version = HttpVersion::Http3;
                return true;
            }
            return false;
        }

        bool TryBuildHttpNegotiatedProtocol(
            const char* alpn,
            bool secure,
            bool datagram,
            bool fallback,
            HttpNegotiatedProtocol& negotiated) noexcept {
            if (alpn == nullptr || *alpn == '\0') return false;

            HttpVersion version = HttpVersion::Http1;
            if (!TryMapHttpAlpn(alpn, version)) return false;

            if (version == HttpVersion::Http3) {
                if (!secure || !datagram || fallback) return false;
            } else if (datagram) {
                return false;
            }

            negotiated = HttpNegotiatedProtocol{ version, alpn, secure, datagram, fallback };
            return true;
        }

        bool IsHttpRequestReplayable(
            const HttpRequest& request,
            const HttpRequestReplayPolicy& policy) noexcept {
            if (IsIdempotentMethod(request.method)) return true;
            if (!policy.allowUnsafeMethodsWithIdempotencyKey) return false;
            for (const auto& header : request.headers) {
                if (IsIdempotencyKeyName(header.name) && !header.value.empty()) {
                    return true;
                }
            }
            return false;
        }

        HttpSession::HttpSession(HttpTransport* transport, HttpRequestHandler* handler)
            : m_impl(new HttpSessionImpl{}) {
            m_impl->transport = transport;
            m_impl->handler = handler;
        }

        HttpSession::~HttpSession() {
            delete m_impl; // 只释放会话状态，不触碰用户适配器
            m_impl = nullptr;
        }

        void HttpSession::SetTransport(HttpTransport* transport) noexcept {
            if (!m_impl) return;
            m_impl->transport = transport;
        }

        void HttpSession::SetHandler(HttpRequestHandler* handler) noexcept {
            if (!m_impl) return;
            m_impl->handler = handler;
        }

        HttpVersion HttpSession::Version() const noexcept {
            return m_impl ? m_impl->version : HttpVersion::Http1;
        }

        void HttpSession::SetVersion(HttpVersion version) noexcept {
            if (!m_impl) return;
            m_impl->version = version;
            m_impl->negotiated.version = version;
            m_impl->negotiated.alpn = ""; // 手动选择不冒充 ALPN 协商
            m_impl->negotiated.secure = false;
            m_impl->negotiated.datagram = false;
            m_impl->negotiated.fallback = false;
        }

        void HttpSession::SetNegotiatedProtocol(
            const HttpNegotiatedProtocol& negotiated) noexcept {
            if (!m_impl) return;
            m_impl->negotiated = negotiated;
            m_impl->version = negotiated.version;
        }

        bool HttpSession::SetNegotiatedAlpn(
            const char* alpn,
            bool secure,
            bool datagram,
            bool fallback) noexcept {
            if (!m_impl) return false;

            HttpNegotiatedProtocol negotiated{};
            if (!TryBuildHttpNegotiatedProtocol(
                    alpn, secure, datagram, fallback, negotiated)) {
                return false;
            }

            SetNegotiatedProtocol(negotiated);
            return true;
        }

        void HttpSession::SetDeadline(
            LikesProgram::Time::Deadline deadline) noexcept {
            if (!m_impl) return;
            m_impl->deadline = deadline;
        }

        bool HttpSession::HasDeadline() const noexcept {
            return m_impl != nullptr && m_impl->deadline.HasDeadline();
        }

        bool HttpSession::DeadlineExpired() const noexcept {
            return m_impl != nullptr && m_impl->deadline.Expired();
        }

        void HttpSession::Cancel() noexcept {
            if (!m_impl) return;
            m_impl->cancelled = true;
        }

        void HttpSession::ResetCancellation() noexcept {
            if (!m_impl) return;
            m_impl->cancelled = false;
        }

        bool HttpSession::IsCancelled() const noexcept {
            return m_impl != nullptr && m_impl->cancelled;
        }

        HttpNegotiatedProtocol HttpSession::NegotiatedProtocol() const noexcept {
            return m_impl ? m_impl->negotiated : HttpNegotiatedProtocol{};
        }

        Result<HttpResponse> HttpSession::Send(const HttpRequest& request) {
            if (!m_impl || m_impl->transport == nullptr) {
                return Status(StatusCode::FailedPrecondition,
                    u"HttpSession transport is not configured");
            }
            if (m_impl->cancelled) {
                return Status(StatusCode::Cancelled,
                    u"HttpSession request was cancelled by the caller");
            }
            if (m_impl->deadline.Expired()) {
                return Status::DeadlineExceeded(
                    u"HttpSession request deadline expired");
            }

            try {
                return m_impl->transport->Exchange(request, m_impl->negotiated);
            }
            catch (...) {
                // 用户传输异常不得越过 Session 的 Result 边界。
                return Status::Internal(u"HttpSession transport callback threw");
            }
        }

        Result<HttpResponse> HttpSession::SendWithFallback(
            const HttpRequest& request,
            HttpVersion fallbackVersion,
            const HttpRequestReplayPolicy& policy) {
            if (policy.maxAttempts == 0) {
                return Status::InvalidArgument(
                    u"HttpSession replay policy requires at least one attempt");
            }
            if (policy.maxAttempts > 2) {
                return Status(StatusCode::OutOfRange,
                    u"HttpSession replay policy supports at most one fallback attempt");
            }
            if (fallbackVersion == HttpVersion::Http3) {
                return Status::InvalidArgument(
                    u"HttpSession fallback must use HTTP/1.1 or HTTP/2");
            }

            auto first = Send(request);
            if (first.IsOk() || policy.maxAttempts == 1 || !m_impl
                || fallbackVersion == m_impl->version
                || !IsRetryableStatus(first.GetStatus().Code(), policy)
                || !IsHttpRequestReplayable(request, policy)) {
                return first;
            }

            const char* alpn = FallbackAlpn(fallbackVersion);
            if (alpn == nullptr) return first;
            const bool secure = m_impl->negotiated.secure;
            HttpNegotiatedProtocol fallback{};
            if (!TryBuildHttpNegotiatedProtocol(
                    alpn, secure, false, true, fallback)) {
                return first;
            }
            SetNegotiatedProtocol(fallback);
            return Send(request);
        }

        Result<HttpResponse> HttpSession::Handle(const HttpRequest& request) {
            if (!m_impl || m_impl->handler == nullptr) {
                return Status(StatusCode::FailedPrecondition,
                    u"HttpSession request handler is not configured");
            }
            if (m_impl->cancelled) {
                return Status(StatusCode::Cancelled,
                    u"HttpSession request was cancelled by the caller");
            }
            if (m_impl->deadline.Expired()) {
                return Status::DeadlineExceeded(
                    u"HttpSession request deadline expired");
            }

            try {
                return m_impl->handler->Handle(request);
            }
            catch (...) {
                // 用户处理器异常转换为稳定内部错误，避免请求循环被击穿。
                return Status::Internal(u"HttpSession request handler callback threw");
            }
        }

        const char* HttpVersionName(HttpVersion version) noexcept {
            switch (version) {
            case HttpVersion::Http1: return "HTTP/1.1";
            case HttpVersion::Http2: return "HTTP/2";
            case HttpVersion::Http3: return "HTTP/3";
            }
            return "UNKNOWN";
        }
    }
}
