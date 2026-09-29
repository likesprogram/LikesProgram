#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpSession.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace LikesProgram {
    namespace Http {
        // A validated, caller-consumable HTTP/3 Alt-Svc advertisement.
        struct HttpAltSvcEntry {
            std::string alpn;
            std::string authority;
            std::uint64_t expiresAt = 0;
        };

        struct HttpAltSvcCacheLimits {
            std::size_t maxEntries = 32;
            std::size_t maxOriginBytes = 256;
            std::size_t maxHeaderBytes = 4096;
        };

        struct HttpAltSvcCacheSnapshot {
            std::size_t entries = 0;
            std::size_t liveEntries = 0;
            std::size_t expiredEntries = 0;
        };

        struct HttpAltSvcSelection {
            HttpVersion version = HttpVersion::Http2;
            std::string alpn;
            std::string authority;
            bool fallback = true;
        };

        // Applies a caller-owned selection to a Session. The Session borrows
        // selection.alpn for the lifetime of the supplied selection object.
        LIKESPROGRAM_HTTP_API bool TryApplyHttpAltSvcSelection(HttpSession& session, const HttpAltSvcSelection& selection, bool secure, bool datagram) noexcept;

        // Bounded origin-scoped Alt-Svc state. It parses only H3 alternatives;
        // DNS, QUIC dialing, timers and connection ownership remain external.
        class LIKESPROGRAM_HTTP_API HttpAltSvcCache {
        public:
            explicit HttpAltSvcCache(HttpAltSvcCacheLimits limits = {});
            ~HttpAltSvcCache();

            HttpAltSvcCache(HttpAltSvcCache&&) noexcept;
            HttpAltSvcCache& operator=(HttpAltSvcCache&&) noexcept;
            HttpAltSvcCache(const HttpAltSvcCache&) = delete;
            HttpAltSvcCache& operator=(const HttpAltSvcCache&) = delete;

            Result<void> SetLimits(HttpAltSvcCacheLimits limits);
            HttpAltSvcCacheLimits Limits() const noexcept;

            // Observe one origin's response Alt-Svc value at caller-supplied
            // monotonic seconds. The clear token removes the origin entry.
            Result<void> Observe(std::string_view origin, std::string_view value, std::uint64_t nowSeconds);
            Result<void> Clear(std::string_view origin);
            Result<std::optional<HttpAltSvcEntry>> Lookup(std::string_view origin, std::uint64_t nowSeconds) const;

            // Select H3 only when a live cached advertisement exists and the
            // caller confirms QUIC availability; otherwise return TCP fallback.
            Result<HttpAltSvcSelection> Select(std::string_view origin, std::uint64_t nowSeconds, HttpVersion fallbackVersion = HttpVersion::Http2, bool allowHttp3 = true) const;

            HttpAltSvcCacheSnapshot Snapshot(std::uint64_t nowSeconds) const noexcept;
            void Reset() noexcept;

        private:
            struct Impl;
            Impl* m_impl = nullptr;
        };
    }
}
