#include <LikesProgram/Http/HttpAltSvc.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Http {
        namespace {
            constexpr std::uint64_t kDefaultMaxAge = 86400;

            std::string_view Trim(std::string_view value) noexcept {
                while (!value.empty()
                    && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
                    value.remove_prefix(1);
                }
                while (!value.empty()
                    && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
                    value.remove_suffix(1);
                }
                return value;
            }

            bool IsToken(std::string_view value) noexcept {
                if (value.empty()) return false;
                for (const unsigned char byte : value) {
                    if (!(std::isalnum(byte) != 0 || byte == '-' || byte == '_'
                        || byte == '.')) {
                        return false;
                    }
                }
                return true;
            }

            bool ParseUnsigned(std::string_view value, std::uint64_t& output) noexcept {
                value = Trim(value);
                if (value.empty()) return false;
                std::uint64_t result = 0;
                for (const unsigned char byte : value) {
                    if (byte < '0' || byte > '9') return false;
                    const auto digit = static_cast<std::uint64_t>(byte - '0');
                    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                        return false;
                    }
                    result = result * 10 + digit;
                }
                output = result;
                return true;
            }

            bool ValidAuthority(std::string_view authority) noexcept {
                if (authority.empty()) return false;
                for (const unsigned char byte : authority) {
                    if (std::isspace(byte) != 0 || byte == '"' || byte == ';'
                        || byte == ',') {
                        return false;
                    }
                }

                const auto colon = authority.rfind(':');
                if (colon == std::string_view::npos) return true;
                if (colon + 1 == authority.size()) return false;
                std::uint64_t port = 0;
                if (!ParseUnsigned(authority.substr(colon + 1), port)) return false;
                return port > 0 && port <= 65535;
            }

            struct ParsedAdvertisement {
                std::string alpn;
                std::string authority;
                std::uint64_t maxAge = kDefaultMaxAge;
            };

            Result<ParsedAdvertisement> ParseAdvertisement(
                std::string_view value) {
                value = Trim(value);
                const auto equals = value.find('=');
                if (equals == std::string_view::npos) {
                    return Status::InvalidArgument(u"Alt-Svc advertisement lacks ALPN assignment");
                }

                const auto alpn = Trim(value.substr(0, equals));
                if (!IsToken(alpn) || !(alpn == "h3" || alpn.starts_with("h3-"))) {
                    return Status::InvalidArgument(u"Alt-Svc advertisement is not an HTTP/3 token");
                }

                auto remainder = Trim(value.substr(equals + 1));
                if (remainder.empty() || remainder.front() != '"') {
                    return Status::InvalidArgument(u"Alt-Svc authority must be quoted");
                }
                remainder.remove_prefix(1);
                const auto quote = remainder.find('"');
                if (quote == std::string_view::npos) {
                    return Status::InvalidArgument(u"Alt-Svc authority quote is unterminated");
                }
                const auto authority = remainder.substr(0, quote);
                if (!ValidAuthority(authority)) {
                    return Status::InvalidArgument(u"Alt-Svc authority is invalid");
                }
                remainder.remove_prefix(quote + 1);

                ParsedAdvertisement parsed{ std::string(alpn), std::string(authority), kDefaultMaxAge };
                while (true) {
                    remainder = Trim(remainder);
                    if (remainder.empty() || remainder.front() == ',') break;
                    if (remainder.front() != ';') {
                        return Status::InvalidArgument(u"Alt-Svc parameter separator is invalid");
                    }
                    remainder.remove_prefix(1);
                    remainder = Trim(remainder);
                    const auto end = remainder.find_first_of("=;,");
                    const auto name = Trim(remainder.substr(0, end));
                    if (!IsToken(name)) {
                        return Status::InvalidArgument(u"Alt-Svc parameter name is invalid");
                    }
                    if (end == std::string_view::npos) break;
                    remainder.remove_prefix(end);
                    if (remainder.front() != '=') continue;
                    remainder.remove_prefix(1);
                    const auto valueEnd = remainder.find_first_of(";,");
                    const auto parameterValue = Trim(remainder.substr(0, valueEnd));
                    if (name == "ma") {
                        if (!ParseUnsigned(parameterValue, parsed.maxAge)) {
                            return Status::InvalidArgument(u"Alt-Svc max-age is invalid");
                        }
                    }
                    if (valueEnd == std::string_view::npos) break;
                    remainder.remove_prefix(valueEnd);
                }
                return parsed;
            }

            bool ValidOrigin(std::string_view origin, const HttpAltSvcCacheLimits& limits) noexcept {
                if (origin.empty() || origin.size() > limits.maxOriginBytes) return false;
                for (const unsigned char byte : origin) {
                    if (std::isspace(byte) != 0 || byte == '\r' || byte == '\n') return false;
                }
                return true;
            }
        }

        struct HttpAltSvcCache::Impl {
            struct CachedEntry {
                std::string origin;
                HttpAltSvcEntry entry;
            };

            HttpAltSvcCacheLimits limits{};
            std::vector<CachedEntry> entries;
        };

        HttpAltSvcCache::HttpAltSvcCache(HttpAltSvcCacheLimits limits)
            : m_impl(new Impl{}) {
            if (limits.maxEntries != 0 && limits.maxOriginBytes != 0
                && limits.maxHeaderBytes != 0) {
                m_impl->limits = limits;
            }
        }

        HttpAltSvcCache::~HttpAltSvcCache() {
            delete m_impl;
            m_impl = nullptr;
        }

        HttpAltSvcCache::HttpAltSvcCache(HttpAltSvcCache&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        HttpAltSvcCache& HttpAltSvcCache::operator=(HttpAltSvcCache&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Result<void> HttpAltSvcCache::SetLimits(HttpAltSvcCacheLimits limits) {
            if (!m_impl || limits.maxEntries == 0 || limits.maxOriginBytes == 0
                || limits.maxHeaderBytes == 0) {
                return Status::InvalidArgument(u"Alt-Svc cache limits must be non-zero");
            }
            if (limits.maxEntries < m_impl->entries.size()) {
                return Status(StatusCode::OutOfRange,
                    u"Alt-Svc cache maxEntries is below the current entry count");
            }
            m_impl->limits = limits;
            return {};
        }

        HttpAltSvcCacheLimits HttpAltSvcCache::Limits() const noexcept {
            return m_impl ? m_impl->limits : HttpAltSvcCacheLimits{};
        }

        Result<void> HttpAltSvcCache::Observe(
            std::string_view origin,
            std::string_view value,
            std::uint64_t nowSeconds) {
            if (!m_impl || !ValidOrigin(origin, m_impl->limits)) {
                return Status::InvalidArgument(u"Alt-Svc origin is invalid or exceeds its limit");
            }
            if (value.size() > m_impl->limits.maxHeaderBytes) {
                return Status(StatusCode::OutOfRange,
                    u"Alt-Svc header exceeds the configured limit");
            }
            if (Trim(value) == "clear") {
                return Clear(origin);
            }

            auto parsed = ParseAdvertisement(value);
            if (!parsed.IsOk()) return parsed.GetStatus();
            const auto maxAge = parsed.Value().maxAge;
            const auto expiresAt = nowSeconds > std::numeric_limits<std::uint64_t>::max() - maxAge
                ? std::numeric_limits<std::uint64_t>::max() : nowSeconds + maxAge;

            auto existing = std::find_if(m_impl->entries.begin(), m_impl->entries.end(),
                [&](const Impl::CachedEntry& item) { return item.origin == origin; });
            if (existing == m_impl->entries.end()
                && m_impl->entries.size() >= m_impl->limits.maxEntries) {
                return Status(StatusCode::ResourceExhausted,
                    u"Alt-Svc cache entry limit is exhausted");
            }

            Impl::CachedEntry item{
                std::string(origin),
                HttpAltSvcEntry{ std::move(parsed.Value().alpn),
                    std::move(parsed.Value().authority), expiresAt }
            };
            if (existing == m_impl->entries.end()) {
                m_impl->entries.push_back(std::move(item));
            } else {
                *existing = std::move(item);
            }
            return {};
        }

        Result<void> HttpAltSvcCache::Clear(std::string_view origin) {
            if (!m_impl || !ValidOrigin(origin, m_impl->limits)) {
                return Status::InvalidArgument(u"Alt-Svc origin is invalid or exceeds its limit");
            }
            m_impl->entries.erase(std::remove_if(m_impl->entries.begin(), m_impl->entries.end(),
                [&](const Impl::CachedEntry& item) { return item.origin == origin; }),
                m_impl->entries.end());
            return {};
        }

        Result<std::optional<HttpAltSvcEntry>> HttpAltSvcCache::Lookup(
            std::string_view origin,
            std::uint64_t nowSeconds) const {
            if (!m_impl || !ValidOrigin(origin, m_impl->limits)) {
                return Status::InvalidArgument(u"Alt-Svc origin is invalid or exceeds its limit");
            }
            const auto existing = std::find_if(m_impl->entries.begin(), m_impl->entries.end(),
                [&](const Impl::CachedEntry& item) { return item.origin == origin; });
            if (existing == m_impl->entries.end() || nowSeconds >= existing->entry.expiresAt) {
                return std::optional<HttpAltSvcEntry>{};
            }
            return std::optional<HttpAltSvcEntry>{ existing->entry };
        }

        Result<HttpAltSvcSelection> HttpAltSvcCache::Select(
            std::string_view origin,
            std::uint64_t nowSeconds,
            HttpVersion fallbackVersion,
            bool allowHttp3) const {
            if (fallbackVersion == HttpVersion::Http3) {
                return Status::InvalidArgument(u"Alt-Svc TCP fallback must use HTTP/1.1 or HTTP/2");
            }
            auto lookup = Lookup(origin, nowSeconds);
            if (!lookup.IsOk()) return lookup.GetStatus();
            HttpAltSvcSelection selection{};
            selection.version = fallbackVersion;
            selection.fallback = true;
            if (!allowHttp3 || !lookup.Value().has_value()) return selection;

            selection.version = HttpVersion::Http3;
            selection.alpn = lookup.Value()->alpn;
            selection.authority = lookup.Value()->authority;
            selection.fallback = false;
            return selection;
        }

        bool TryApplyHttpAltSvcSelection(
            HttpSession& session,
            const HttpAltSvcSelection& selection,
            bool secure,
            bool datagram) noexcept {
            if (selection.version == HttpVersion::Http3) {
                if (selection.fallback || selection.alpn.empty()
                    || selection.authority.empty() || !secure || !datagram) {
                    return false;
                }
                return session.SetNegotiatedAlpn(
                    selection.alpn.c_str(), secure, datagram, false);
            }

            if (!selection.fallback || !selection.alpn.empty() || datagram) return false;
            const char* alpn = selection.version == HttpVersion::Http1
                ? "http/1.1" : selection.version == HttpVersion::Http2 ? "h2" : nullptr;
            if (alpn == nullptr) return false;
            return session.SetNegotiatedAlpn(alpn, secure, false, true);
        }

        HttpAltSvcCacheSnapshot HttpAltSvcCache::Snapshot(
            std::uint64_t nowSeconds) const noexcept {
            HttpAltSvcCacheSnapshot snapshot{};
            if (!m_impl) return snapshot;
            snapshot.entries = m_impl->entries.size();
            for (const auto& item : m_impl->entries) {
                if (nowSeconds < item.entry.expiresAt) ++snapshot.liveEntries;
                else ++snapshot.expiredEntries;
            }
            return snapshot;
        }

        void HttpAltSvcCache::Reset() noexcept {
            if (m_impl) m_impl->entries.clear();
        }
    }
}
