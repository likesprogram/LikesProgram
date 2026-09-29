#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http1.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace LikesProgram {
    namespace Http {
        struct HttpUri {
            std::string scheme;
            std::string authority;
            std::string host;
            std::string pathAndQuery;
            std::uint16_t port = 0;
            bool explicitPort = false;

            bool Secure() const noexcept { return scheme == "https"; }
        };

        LIKESPROGRAM_HTTP_API Result<HttpUri> ParseHttpUri(std::string_view uri);
        LIKESPROGRAM_HTTP_API Result<std::string> ResolveHttpUri(std::string_view baseUri, std::string_view reference);
        LIKESPROGRAM_HTTP_API std::string HttpUriOrigin(const HttpUri& uri);

        struct HttpRedirectPolicy {
            std::size_t maxRedirects = 10;
            bool allowHttpsDowngrade = false;
            bool preservePostOn301And302 = false;
        };

        struct HttpRedirectDecision {
            bool follow = false;
            bool originChanged = false;
            bool bodyPreserved = true;
            std::string uri;
            HttpRequest request;
        };

        // Builds the next request only. The caller owns loop detection,
        // scheduling, transport selection, credential policy and actual I/O.
        LIKESPROGRAM_HTTP_API Result<HttpRedirectDecision> PrepareHttpRedirect(std::string_view currentUri, const HttpRequest& request, const HttpResponse& response, std::size_t redirectsFollowed, const HttpRedirectPolicy& policy = {});

        enum class HttpCookieSameSite : std::uint8_t {
            Unspecified,
            Lax,
            Strict,
            None
        };

        struct HttpCookie {
            std::string name;
            std::string value;
            std::string domain;
            std::string path;
            std::uint64_t expiresAt = 0;
            std::uint64_t creationSequence = 0;
            HttpCookieSameSite sameSite = HttpCookieSameSite::Unspecified;
            bool persistent = false;
            bool hostOnly = true;
            bool secure = false;
            bool httpOnly = false;
        };

        struct HttpCookieJarLimits {
            std::size_t maxCookies = 256;
            std::size_t maxCookiesPerDomain = 50;
            std::size_t maxCookieBytes = 4096;
            std::size_t maxHeaderBytes = 64 * 1024;
            std::size_t maxDomainBytes = 255;
            std::size_t maxPathBytes = 1024;
        };

        struct HttpCookieJarSnapshot {
            std::size_t cookies = 0;
            std::size_t persistentCookies = 0;
            std::size_t secureCookies = 0;
            std::size_t expiredCookies = 0;
            std::size_t storedBytes = 0;
        };

        struct HttpCookieRequestContext {
            bool sameSite = true;
            bool topLevelNavigation = false;
            bool safeMethod = true;
        };

        // Optional public-suffix/organization policy. Domain-match validation
        // is always enforced; a supplied policy can reject public suffixes.
        class LIKESPROGRAM_HTTP_API HttpCookieDomainPolicy {
        public:
            virtual ~HttpCookieDomainPolicy() = default;
            virtual bool AcceptDomain(std::string_view requestHost, std::string_view cookieDomain) const noexcept = 0;
        };

        class LIKESPROGRAM_HTTP_API HttpCookieJar {
        public:
            explicit HttpCookieJar(HttpCookieJarLimits limits = {}, const HttpCookieDomainPolicy* domainPolicy = nullptr);
            ~HttpCookieJar();

            HttpCookieJar(HttpCookieJar&&) noexcept;
            HttpCookieJar& operator=(HttpCookieJar&&) noexcept;
            HttpCookieJar(const HttpCookieJar&) = delete;
            HttpCookieJar& operator=(const HttpCookieJar&) = delete;

            Result<void> Store(std::string_view requestUri, std::string_view setCookieValue, std::uint64_t nowUnixSeconds);
            Result<std::string> CookieHeader(std::string_view requestUri, std::uint64_t nowUnixSeconds, const HttpCookieRequestContext& context = {}) const;
            Result<void> ClearExpired(std::uint64_t nowUnixSeconds);
            HttpCookieJarSnapshot Snapshot(std::uint64_t nowUnixSeconds) const noexcept;
            HttpCookieJarLimits Limits() const noexcept;
            void Reset() noexcept;

        private:
            struct Impl;
            Impl* m_impl = nullptr;
        };

        struct HttpByteRangeSpec {
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            bool hasFirst = false;
            bool hasLast = false;
        };

        struct HttpResolvedByteRange {
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            std::uint64_t length = 0;
        };

        LIKESPROGRAM_HTTP_API Result<std::vector<HttpByteRangeSpec>> ParseHttpByteRanges(std::string_view value, std::size_t maxRanges = 16);
        LIKESPROGRAM_HTTP_API Result<std::vector<HttpResolvedByteRange>> ResolveHttpByteRanges(std::span<const HttpByteRangeSpec> ranges, std::uint64_t representationLength);
        LIKESPROGRAM_HTTP_API std::string BuildHttpContentRange(const HttpResolvedByteRange& range, std::uint64_t representationLength);
        LIKESPROGRAM_HTTP_API std::string BuildHttpUnsatisfiedContentRange(std::uint64_t representationLength);

        struct HttpContentDecodingLimits {
            std::size_t maxCodings = 4;
            std::size_t maxDecodedBytes = 64 * 1024 * 1024;
        };

        class LIKESPROGRAM_HTTP_API HttpContentDecoder {
        public:
            virtual ~HttpContentDecoder() = default;
            virtual Result<std::vector<std::uint8_t>> Decode(std::string_view coding, std::span<const std::uint8_t> input, std::size_t maxOutputBytes) = 0;
        };

        // Applies Content-Encoding in reverse order and normalizes the decoded
        // response. Concrete gzip/br/zstd implementations remain caller-owned.
        LIKESPROGRAM_HTTP_API Result<HttpResponse> DecodeHttpContent(
            const HttpResponse& response,
            HttpContentDecoder& decoder,
            const HttpContentDecodingLimits& limits = {});

        struct HttpMultipartLimits {
            std::size_t maxParts = 128;
            std::size_t maxHeaderBytesPerPart = 64 * 1024;
            std::size_t maxBodyBytesPerPart = 16 * 1024 * 1024;
            std::size_t maxTotalBytes = 64 * 1024 * 1024;
            std::size_t maxBoundaryBytes = 200;
        };

        struct HttpMultipartPart {
            std::vector<HttpHeader> headers;
            std::vector<std::uint8_t> body;
            std::string name;
            std::string filename;
        };

        LIKESPROGRAM_HTTP_API Result<std::string> ParseHttpMultipartBoundary(std::string_view contentType, std::size_t maxBoundaryBytes = 200);
        LIKESPROGRAM_HTTP_API Result<std::vector<HttpMultipartPart>> ParseHttpMultipart(std::string_view contentType, std::span<const std::uint8_t> body, const HttpMultipartLimits& limits = {});
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttpMultipart(std::string_view boundary, std::span<const HttpMultipartPart> parts, const HttpMultipartLimits& limits = {});

        struct HttpAuthenticationChallenge {
            std::string scheme;
            std::string credentials;
        };

        LIKESPROGRAM_HTTP_API Result<std::vector<HttpAuthenticationChallenge>> ParseHttpAuthenticationChallenges(std::string_view value, std::size_t maxChallenges = 16, std::size_t maxBytes = 16 * 1024);
        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttpBasicAuthorization(std::string_view user, std::string_view password, std::size_t maxCredentialBytes = 16 * 1024);
        LIKESPROGRAM_HTTP_API Result<std::string> BuildHttpBearerAuthorization(std::string_view token, std::size_t maxTokenBytes = 16 * 1024);

        struct HttpCacheControl {
            bool noStore = false;
            bool noCache = false;
            bool mustRevalidate = false;
            bool proxyRevalidate = false;
            bool isPrivate = false;
            bool isPublic = false;
            bool immutable = false;
            std::optional<std::uint64_t> maxAge;
            std::optional<std::uint64_t> sharedMaxAge;
            std::optional<std::uint64_t> staleWhileRevalidate;
            std::optional<std::uint64_t> staleIfError;
        };

        struct HttpCacheValidators {
            std::string etag;
            std::string lastModified;
        };

        struct HttpCacheEvaluation {
            bool cacheable = false;
            bool requiresRevalidation = false;
            std::optional<std::uint64_t> freshnessLifetime;
        };

        LIKESPROGRAM_HTTP_API Result<HttpCacheControl> ParseHttpCacheControl(std::span<const HttpHeader> headers, std::size_t maxDirectives = 64);
        LIKESPROGRAM_HTTP_API Result<void> ApplyHttpCacheValidators(HttpRequest& request, const HttpCacheValidators& validators);
        LIKESPROGRAM_HTTP_API Result<HttpCacheEvaluation> EvaluateHttpCacheResponse(const HttpRequest& request, const HttpResponse& response, bool sharedCache = false);
    }
}
