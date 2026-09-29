#include <LikesProgram/Http/HttpWebsite.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

namespace LikesProgram {
    namespace Http {
        namespace {
            constexpr std::uint64_t kSessionCookie = 0;

            char AsciiLower(char value) noexcept {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : value;
            }

            std::string Lower(std::string_view value) {
                std::string result(value);
                for (char& byte : result) byte = AsciiLower(byte);
                return result;
            }

            bool EqualIgnoreCase(std::string_view left, std::string_view right) noexcept {
                if (left.size() != right.size()) return false;
                for (std::size_t index = 0; index < left.size(); ++index) {
                    if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
                }
                return true;
            }

            std::string_view Trim(std::string_view value) noexcept {
                while (!value.empty()
                    && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty()
                    && (value.back() == ' ' || value.back() == '\t')) {
                    value.remove_suffix(1);
                }
                return value;
            }

            bool HasInvalidUriByte(std::string_view value) noexcept {
                for (std::size_t index = 0; index < value.size(); ++index) {
                    const auto byte = static_cast<unsigned char>(value[index]);
                    if (byte <= 0x20 || byte == 0x7F) return true;
                    if (byte == '%') {
                        if (index + 2 >= value.size()
                            || std::isxdigit(static_cast<unsigned char>(value[index + 1])) == 0
                            || std::isxdigit(static_cast<unsigned char>(value[index + 2])) == 0) {
                            return true;
                        }
                        index += 2;
                    }
                }
                return false;
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

            bool ParseSigned(std::string_view value, std::int64_t& output) noexcept {
                value = Trim(value);
                if (value.empty()) return false;
                bool negative = false;
                if (value.front() == '+' || value.front() == '-') {
                    negative = value.front() == '-';
                    value.remove_prefix(1);
                }
                std::uint64_t magnitude = 0;
                if (!ParseUnsigned(value, magnitude)) return false;
                const auto positiveLimit = static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max());
                if ((!negative && magnitude > positiveLimit)
                    || (negative && magnitude > positiveLimit + 1)) {
                    return false;
                }
                if (negative && magnitude == positiveLimit + 1) {
                    output = std::numeric_limits<std::int64_t>::min();
                } else {
                    output = negative
                        ? -static_cast<std::int64_t>(magnitude)
                        : static_cast<std::int64_t>(magnitude);
                }
                return true;
            }

            bool ValidScheme(std::string_view scheme) noexcept {
                if (scheme.empty() || std::isalpha(
                        static_cast<unsigned char>(scheme.front())) == 0) {
                    return false;
                }
                for (const unsigned char byte : scheme.substr(1)) {
                    if (std::isalnum(byte) == 0 && byte != '+' && byte != '-'
                        && byte != '.') {
                        return false;
                    }
                }
                return true;
            }

            std::string NormalizePath(std::string_view path) {
                const bool trailingSlash = path.size() > 1 && path.back() == '/';
                std::vector<std::string_view> segments;
                std::size_t begin = path.starts_with('/') ? 1 : 0;
                while (begin <= path.size()) {
                    const auto slash = path.find('/', begin);
                    const auto end = slash == std::string_view::npos ? path.size() : slash;
                    const auto segment = path.substr(begin, end - begin);
                    if (segment == "..") {
                        if (!segments.empty()) segments.pop_back();
                    } else if (!segment.empty() && segment != ".") {
                        segments.push_back(segment);
                    }
                    if (slash == std::string_view::npos) break;
                    begin = slash + 1;
                }

                std::string result = "/";
                for (std::size_t index = 0; index < segments.size(); ++index) {
                    if (index != 0) result.push_back('/');
                    result.append(segments[index]);
                }
                if (trailingSlash && result.back() != '/') result.push_back('/');
                return result;
            }

            std::string NormalizePathAndQuery(std::string_view value) {
                const auto query = value.find('?');
                const auto path = query == std::string_view::npos
                    ? value : value.substr(0, query);
                std::string result = NormalizePath(path.empty() ? "/" : path);
                if (query != std::string_view::npos) result.append(value.substr(query));
                return result;
            }

            std::string AuthorityForHost(
                std::string_view host,
                std::uint16_t port,
                bool explicitPort) {
                std::string authority;
                if (host.find(':') != std::string_view::npos) {
                    authority.push_back('[');
                    authority.append(host);
                    authority.push_back(']');
                } else {
                    authority.assign(host);
                }
                if (explicitPort) {
                    authority.push_back(':');
                    authority.append(std::to_string(port));
                }
                return authority;
            }

            bool SameOrigin(const HttpUri& left, const HttpUri& right) noexcept {
                return left.scheme == right.scheme && left.host == right.host
                    && left.port == right.port;
            }

            bool HeaderNameIs(const HttpHeader& header, std::string_view name) noexcept {
                return EqualIgnoreCase(header.name, name);
            }

            std::vector<std::string_view> HeaderValues(
                const std::vector<HttpHeader>& headers,
                std::string_view name) {
                std::vector<std::string_view> result;
                for (const auto& header : headers) {
                    if (HeaderNameIs(header, name)) result.push_back(header.value);
                }
                return result;
            }

            void RemoveHeaders(
                std::vector<HttpHeader>& headers,
                std::span<const std::string_view> names) {
                std::erase_if(headers, [&](const HttpHeader& header) {
                    return std::ranges::any_of(names, [&](std::string_view name) {
                        return HeaderNameIs(header, name);
                    });
                });
            }

            void SetHeader(
                std::vector<HttpHeader>& headers,
                std::string_view name,
                std::string value) {
                const std::array<std::string_view, 1> names{ name };
                RemoveHeaders(headers, names);
                headers.push_back({ std::string(name), std::move(value) });
            }

            bool IsRedirectStatus(int status) noexcept {
                return status == 301 || status == 302 || status == 303
                    || status == 307 || status == 308;
            }

            bool ValidCookieName(std::string_view name) noexcept {
                if (name.empty()) return false;
                constexpr std::string_view separators = "()<>@,;:\\\"/[]?={} \t";
                for (const unsigned char byte : name) {
                    if (byte <= 0x20 || byte >= 0x7F
                        || separators.find(static_cast<char>(byte)) != std::string_view::npos) {
                        return false;
                    }
                }
                return true;
            }

            bool ValidCookieValue(std::string_view value) noexcept {
                for (const unsigned char byte : value) {
                    if (byte < 0x21 || byte == '"' || byte == ',' || byte == ';'
                        || byte == '\\' || byte >= 0x7F) {
                        return false;
                    }
                }
                return true;
            }

            bool IsIpAddress(std::string_view host) noexcept {
                if (host.find(':') != std::string_view::npos) return true;
                bool dot = false;
                for (const unsigned char byte : host) {
                    if (byte == '.') {
                        dot = true;
                    } else if (byte < '0' || byte > '9') {
                        return false;
                    }
                }
                return dot;
            }

            bool DomainMatch(std::string_view host, std::string_view domain) noexcept {
                if (host == domain) return true;
                return host.size() > domain.size()
                    && host.ends_with(domain)
                    && host[host.size() - domain.size() - 1] == '.';
            }

            std::string DefaultCookiePath(std::string_view pathAndQuery) {
                const auto query = pathAndQuery.find('?');
                const auto path = pathAndQuery.substr(0, query);
                if (path.empty() || path.front() != '/') return "/";
                const auto slash = path.rfind('/');
                if (slash == 0 || slash == std::string_view::npos) return "/";
                return std::string(path.substr(0, slash));
            }

            bool PathMatch(std::string_view requestPath, std::string_view cookiePath) noexcept {
                if (requestPath == cookiePath) return true;
                if (!requestPath.starts_with(cookiePath)) return false;
                return cookiePath.ends_with('/')
                    || (requestPath.size() > cookiePath.size()
                        && requestPath[cookiePath.size()] == '/');
            }

            int MonthNumber(std::string_view token) noexcept {
                static constexpr std::array<std::string_view, 12> months{
                    "jan", "feb", "mar", "apr", "may", "jun",
                    "jul", "aug", "sep", "oct", "nov", "dec"
                };
                const auto lower = Lower(token);
                for (std::size_t index = 0; index < months.size(); ++index) {
                    if (lower == months[index]) return static_cast<int>(index + 1);
                }
                return 0;
            }

            bool IsLeapYear(int year) noexcept {
                return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
            }

            int DaysInMonth(int year, int month) noexcept {
                static constexpr std::array<int, 12> days{
                    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
                };
                return month == 2 && IsLeapYear(year)
                    ? 29 : days[static_cast<std::size_t>(month - 1)];
            }

            std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept {
                year -= month <= 2;
                const int era = (year >= 0 ? year : year - 399) / 400;
                const auto yearOfEra = static_cast<unsigned>(year - era * 400);
                const auto dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
                    + day - 1;
                const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4
                    - yearOfEra / 100 + dayOfYear;
                return static_cast<std::int64_t>(era) * 146097
                    + static_cast<std::int64_t>(dayOfEra) - 719468;
            }

            bool ParseCookieDate(std::string_view value, std::uint64_t& output) {
                std::vector<std::string_view> tokens;
                std::size_t begin = 0;
                while (begin < value.size()) {
                    while (begin < value.size()
                        && std::isalnum(static_cast<unsigned char>(value[begin])) == 0) {
                        ++begin;
                    }
                    if (begin == value.size()) break;
                    std::size_t end = begin;
                    while (end < value.size()
                        && (std::isalnum(static_cast<unsigned char>(value[end])) != 0
                            || value[end] == ':')) {
                        ++end;
                    }
                    tokens.push_back(value.substr(begin, end - begin));
                    begin = end;
                }

                int day = 0;
                int month = 0;
                int year = 0;
                int hour = -1;
                int minute = -1;
                int second = -1;
                for (const auto token : tokens) {
                    if (token.find(':') != std::string_view::npos && hour < 0) {
                        const auto first = token.find(':');
                        const auto secondColon = token.find(':', first + 1);
                        std::uint64_t h = 0, m = 0, s = 0;
                        if (secondColon == std::string_view::npos
                            || !ParseUnsigned(token.substr(0, first), h)
                            || !ParseUnsigned(token.substr(first + 1, secondColon - first - 1), m)
                            || !ParseUnsigned(token.substr(secondColon + 1), s)
                            || h > 23 || m > 59 || s > 59) {
                            return false;
                        }
                        hour = static_cast<int>(h);
                        minute = static_cast<int>(m);
                        second = static_cast<int>(s);
                        continue;
                    }
                    const int parsedMonth = MonthNumber(token);
                    if (parsedMonth != 0 && month == 0) {
                        month = parsedMonth;
                        continue;
                    }
                    std::uint64_t number = 0;
                    if (!ParseUnsigned(token, number)) continue;
                    if (day == 0 && token.size() <= 2 && number >= 1 && number <= 31) {
                        day = static_cast<int>(number);
                    } else if (year == 0 && token.size() >= 2 && token.size() <= 4) {
                        year = static_cast<int>(number);
                    }
                }
                if (year >= 0 && year <= 69) year += 2000;
                else if (year >= 70 && year <= 99) year += 1900;
                if (year < 1601 || month == 0 || day == 0 || hour < 0
                    || day > DaysInMonth(year, month)) {
                    return false;
                }
                const auto days = DaysFromCivil(
                    year, static_cast<unsigned>(month), static_cast<unsigned>(day));
                if (days < 0) {
                    output = 0;
                    return true;
                }
                const auto seconds = static_cast<std::uint64_t>(days) * 86400
                    + static_cast<std::uint64_t>(hour) * 3600
                    + static_cast<std::uint64_t>(minute) * 60
                    + static_cast<std::uint64_t>(second);
                output = seconds;
                return true;
            }

            std::size_t CookieStoredBytes(const HttpCookie& cookie) noexcept {
                return cookie.name.size() + cookie.value.size()
                    + cookie.domain.size() + cookie.path.size();
            }
        }

        Result<HttpUri> ParseHttpUri(std::string_view uri) {
            const auto fragment = uri.find('#');
            if (fragment != std::string_view::npos) uri = uri.substr(0, fragment);
            const auto schemeEnd = uri.find("://");
            if (schemeEnd == std::string_view::npos) {
                return Status::InvalidArgument(u"HTTP URI must be absolute");
            }
            const auto schemeView = uri.substr(0, schemeEnd);
            if (!ValidScheme(schemeView)) {
                return Status::InvalidArgument(u"HTTP URI scheme is invalid");
            }
            const auto scheme = Lower(schemeView);
            if (scheme != "http" && scheme != "https") {
                return Status::InvalidArgument(u"HTTP URI scheme must be http or https");
            }

            const auto authorityBegin = schemeEnd + 3;
            const auto authorityEnd = uri.find_first_of("/?", authorityBegin);
            const auto authorityView = uri.substr(authorityBegin,
                authorityEnd == std::string_view::npos
                    ? uri.size() - authorityBegin : authorityEnd - authorityBegin);
            if (authorityView.empty() || HasInvalidUriByte(authorityView)
                || authorityView.find('@') != std::string_view::npos) {
                return Status::InvalidArgument(u"HTTP URI authority is invalid");
            }

            std::string_view hostView;
            std::string_view portView;
            bool explicitPort = false;
            if (authorityView.front() == '[') {
                const auto bracket = authorityView.find(']');
                if (bracket == std::string_view::npos || bracket == 1) {
                    return Status::InvalidArgument(u"HTTP URI IPv6 authority is invalid");
                }
                hostView = authorityView.substr(1, bracket - 1);
                if (bracket + 1 < authorityView.size()) {
                    if (authorityView[bracket + 1] != ':') {
                        return Status::InvalidArgument(u"HTTP URI IPv6 port is invalid");
                    }
                    portView = authorityView.substr(bracket + 2);
                    explicitPort = true;
                }
            } else {
                const auto colon = authorityView.rfind(':');
                if (colon != std::string_view::npos) {
                    if (authorityView.find(':') != colon) {
                        return Status::InvalidArgument(u"HTTP URI IPv6 host must be bracketed");
                    }
                    hostView = authorityView.substr(0, colon);
                    portView = authorityView.substr(colon + 1);
                    explicitPort = true;
                } else {
                    hostView = authorityView;
                }
            }
            if (hostView.empty()) {
                return Status::InvalidArgument(u"HTTP URI host is empty");
            }
            std::uint64_t parsedPort = scheme == "https" ? 443 : 80;
            if (explicitPort && (!ParseUnsigned(portView, parsedPort)
                    || parsedPort == 0 || parsedPort > 65535)) {
                return Status::InvalidArgument(u"HTTP URI port is invalid");
            }

            std::string_view pathAndQuery = authorityEnd == std::string_view::npos
                ? std::string_view{} : uri.substr(authorityEnd);
            if (HasInvalidUriByte(pathAndQuery)) {
                return Status::InvalidArgument(u"HTTP URI path or query is invalid");
            }
            if (pathAndQuery.starts_with('?')) {
                std::string prefixed = "/";
                prefixed.append(pathAndQuery);
                pathAndQuery = {};
                HttpUri result;
                result.scheme = scheme;
                result.host = Lower(hostView);
                result.port = static_cast<std::uint16_t>(parsedPort);
                result.explicitPort = explicitPort;
                result.authority = AuthorityForHost(
                    result.host, result.port, result.explicitPort);
                result.pathAndQuery = std::move(prefixed);
                return result;
            }

            HttpUri result;
            result.scheme = scheme;
            result.host = Lower(hostView);
            result.port = static_cast<std::uint16_t>(parsedPort);
            result.explicitPort = explicitPort;
            result.authority = AuthorityForHost(result.host, result.port, explicitPort);
            result.pathAndQuery = NormalizePathAndQuery(pathAndQuery.empty() ? "/" : pathAndQuery);
            return result;
        }

        Result<std::string> ResolveHttpUri(
            std::string_view baseUri,
            std::string_view reference) {
            const auto base = ParseHttpUri(baseUri);
            if (!base.IsOk()) return base.PropagateFailure<std::string>();
            const auto fragment = reference.find('#');
            if (fragment != std::string_view::npos) reference = reference.substr(0, fragment);
            const auto absoluteMarker = reference.find("://");
            if (absoluteMarker != std::string_view::npos
                && ValidScheme(reference.substr(0, absoluteMarker))) {
                const auto absolute = ParseHttpUri(reference);
                if (!absolute.IsOk()) return absolute.PropagateFailure<std::string>();
                return absolute.Value().scheme + "://" + absolute.Value().authority
                    + absolute.Value().pathAndQuery;
            }
            if (reference.starts_with("//")) {
                return ResolveHttpUri(base.Value().scheme + ":" + std::string(reference), "");
            }
            if (HasInvalidUriByte(reference)) {
                return Status::InvalidArgument(u"HTTP redirect reference is invalid");
            }

            std::string pathAndQuery;
            if (reference.empty()) {
                pathAndQuery = base.Value().pathAndQuery;
            } else if (reference.front() == '?') {
                const auto query = base.Value().pathAndQuery.find('?');
                pathAndQuery = base.Value().pathAndQuery.substr(0, query);
                pathAndQuery.append(reference);
            } else if (reference.front() == '/') {
                pathAndQuery = NormalizePathAndQuery(reference);
            } else {
                const auto query = base.Value().pathAndQuery.find('?');
                const auto basePath = base.Value().pathAndQuery.substr(0, query);
                const auto slash = basePath.rfind('/');
                std::string merged = slash == std::string_view::npos
                    ? "/" : std::string(basePath.substr(0, slash + 1));
                merged.append(reference);
                pathAndQuery = NormalizePathAndQuery(merged);
            }
            return base.Value().scheme + "://" + base.Value().authority + pathAndQuery;
        }

        std::string HttpUriOrigin(const HttpUri& uri) {
            const bool defaultPort = (uri.scheme == "http" && uri.port == 80)
                || (uri.scheme == "https" && uri.port == 443);
            return uri.scheme + "://" + AuthorityForHost(
                uri.host, uri.port, !defaultPort);
        }

        Result<HttpRedirectDecision> PrepareHttpRedirect(
            std::string_view currentUri,
            const HttpRequest& request,
            const HttpResponse& response,
            std::size_t redirectsFollowed,
            const HttpRedirectPolicy& policy) {
            HttpRedirectDecision decision;
            decision.request = request;
            decision.uri = std::string(currentUri);
            if (!IsRedirectStatus(response.statusCode)) return decision;
            if (policy.maxRedirects == 0 || redirectsFollowed >= policy.maxRedirects) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP redirect limit is exhausted");
            }
            const auto locations = HeaderValues(response.headers, "location");
            if (locations.size() != 1 || Trim(locations.front()).empty()) {
                return Status::InvalidArgument(
                    u"HTTP redirect requires exactly one non-empty Location");
            }
            const auto resolved = ResolveHttpUri(currentUri, Trim(locations.front()));
            if (!resolved.IsOk()) return resolved.PropagateFailure<HttpRedirectDecision>();
            const auto current = ParseHttpUri(currentUri);
            const auto target = ParseHttpUri(resolved.Value());
            if (!current.IsOk()) return current.PropagateFailure<HttpRedirectDecision>();
            if (!target.IsOk()) return target.PropagateFailure<HttpRedirectDecision>();
            if (current.Value().Secure() && !target.Value().Secure()
                && !policy.allowHttpsDowngrade) {
                return Status(StatusCode::PermissionDenied,
                    u"HTTP redirect would downgrade HTTPS to HTTP");
            }

            decision.follow = true;
            decision.uri = resolved.Value();
            decision.originChanged = !SameOrigin(current.Value(), target.Value());
            decision.request.target = target.Value().pathAndQuery;
            SetHeader(decision.request.headers, "Host", target.Value().authority);
            if (decision.originChanged) {
                const std::array<std::string_view, 3> sensitive{
                    "authorization", "proxy-authorization", "cookie"
                };
                RemoveHeaders(decision.request.headers, sensitive);
            }

            const bool switchToGet = response.statusCode == 303
                ? !EqualIgnoreCase(request.method, "HEAD")
                : ((response.statusCode == 301 || response.statusCode == 302)
                    && EqualIgnoreCase(request.method, "POST")
                    && !policy.preservePostOn301And302);
            if (switchToGet) {
                decision.request.method = "GET";
                decision.request.body.clear();
                decision.bodyPreserved = false;
                const std::array<std::string_view, 4> entityHeaders{
                    "content-length", "content-type", "content-encoding", "transfer-encoding"
                };
                RemoveHeaders(decision.request.headers, entityHeaders);
            }
            return decision;
        }

        struct HttpCookieJar::Impl {
            HttpCookieJarLimits limits{};
            const HttpCookieDomainPolicy* domainPolicy = nullptr;
            std::vector<HttpCookie> cookies;
            std::uint64_t nextSequence = 1;
        };

        HttpCookieJar::HttpCookieJar(
            HttpCookieJarLimits limits,
            const HttpCookieDomainPolicy* domainPolicy)
            : m_impl(new Impl{}) {
            if (limits.maxCookies != 0 && limits.maxCookiesPerDomain != 0
                && limits.maxCookieBytes != 0 && limits.maxHeaderBytes != 0
                && limits.maxDomainBytes != 0
                && limits.maxPathBytes != 0) {
                m_impl->limits = limits;
            }
            m_impl->domainPolicy = domainPolicy;
        }

        HttpCookieJar::~HttpCookieJar() {
            delete m_impl;
            m_impl = nullptr;
        }

        HttpCookieJar::HttpCookieJar(HttpCookieJar&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        HttpCookieJar& HttpCookieJar::operator=(HttpCookieJar&& other) noexcept {
            if (this == &other) return *this;
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Result<void> HttpCookieJar::Store(
            std::string_view requestUri,
            std::string_view setCookieValue,
            std::uint64_t nowUnixSeconds) {
            if (!m_impl) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP cookie jar is moved from");
            }
            if (setCookieValue.size() > m_impl->limits.maxCookieBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"Set-Cookie exceeds the configured byte limit");
            }
            const auto uri = ParseHttpUri(requestUri);
            if (!uri.IsOk()) return uri.GetStatus();
            if (uri.Value().host.size() > m_impl->limits.maxDomainBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"Cookie request host exceeds the configured limit");
            }

            const auto separator = setCookieValue.find(';');
            const auto pair = Trim(setCookieValue.substr(0, separator));
            const auto equals = pair.find('=');
            if (equals == std::string_view::npos) {
                return Status::InvalidArgument(u"Set-Cookie lacks a name-value pair");
            }
            HttpCookie cookie;
            cookie.name = std::string(Trim(pair.substr(0, equals)));
            auto cookieValue = Trim(pair.substr(equals + 1));
            if (!cookieValue.empty() && cookieValue.front() == '"') {
                if (cookieValue.size() < 2 || cookieValue.back() != '"') {
                    return Status::InvalidArgument(u"Set-Cookie quoted value is unterminated");
                }
                cookieValue.remove_prefix(1);
                cookieValue.remove_suffix(1);
            }
            cookie.value = std::string(cookieValue);
            if (!ValidCookieName(cookie.name) || !ValidCookieValue(cookie.value)) {
                return Status::InvalidArgument(u"Set-Cookie name or value is invalid");
            }
            cookie.domain = uri.Value().host;
            cookie.path = DefaultCookiePath(uri.Value().pathAndQuery);

            std::optional<std::int64_t> maxAge;
            std::optional<std::uint64_t> expires;
            std::size_t cursor = separator;
            while (cursor != std::string_view::npos && cursor < setCookieValue.size()) {
                ++cursor;
                const auto next = setCookieValue.find(';', cursor);
                const auto attribute = Trim(setCookieValue.substr(cursor,
                    next == std::string_view::npos
                        ? setCookieValue.size() - cursor : next - cursor));
                const auto attributeEquals = attribute.find('=');
                const auto name = Lower(Trim(attribute.substr(0, attributeEquals)));
                auto value = attributeEquals == std::string_view::npos
                    ? std::string_view{} : Trim(attribute.substr(attributeEquals + 1));
                if (name == "secure") {
                    cookie.secure = true;
                } else if (name == "httponly") {
                    cookie.httpOnly = true;
                } else if (name == "domain") {
                    if (value.empty()) {
                        return Status::InvalidArgument(u"Cookie Domain is empty");
                    }
                    while (value.starts_with('.')) value.remove_prefix(1);
                    const auto domain = Lower(value);
                    if (domain.empty() || domain.ends_with('.') || IsIpAddress(uri.Value().host)
                        || !DomainMatch(uri.Value().host, domain)
                        || domain.size() > m_impl->limits.maxDomainBytes) {
                        return Status(StatusCode::PermissionDenied,
                            u"Cookie Domain does not match the request host");
                    }
                    if (m_impl->domainPolicy != nullptr
                        && !m_impl->domainPolicy->AcceptDomain(uri.Value().host, domain)) {
                        return Status(StatusCode::PermissionDenied,
                            u"Cookie Domain was rejected by caller policy");
                    }
                    cookie.domain = domain;
                    cookie.hostOnly = false;
                } else if (name == "path") {
                    if (!value.empty() && value.front() == '/') cookie.path = std::string(value);
                } else if (name == "max-age") {
                    std::int64_t parsed = 0;
                    if (!ParseSigned(value, parsed)) {
                        return Status::InvalidArgument(u"Cookie Max-Age is invalid");
                    }
                    maxAge = parsed;
                } else if (name == "expires") {
                    std::uint64_t parsed = 0;
                    if (ParseCookieDate(value, parsed)) expires = parsed;
                } else if (name == "samesite") {
                    if (EqualIgnoreCase(value, "lax")) cookie.sameSite = HttpCookieSameSite::Lax;
                    else if (EqualIgnoreCase(value, "strict")) cookie.sameSite = HttpCookieSameSite::Strict;
                    else if (EqualIgnoreCase(value, "none")) cookie.sameSite = HttpCookieSameSite::None;
                }
                cursor = next;
            }
            if (cookie.path.size() > m_impl->limits.maxPathBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"Cookie Path exceeds the configured limit");
            }
            if ((cookie.secure && !uri.Value().Secure())
                || (cookie.sameSite == HttpCookieSameSite::None && !cookie.secure)
                || (cookie.name.starts_with("__Secure-") && !cookie.secure)
                || (cookie.name.starts_with("__Host-")
                    && (!cookie.secure || !cookie.hostOnly || cookie.path != "/"))) {
                return Status(StatusCode::PermissionDenied,
                    u"Cookie security attributes are inconsistent with the request");
            }

            if (maxAge.has_value()) {
                cookie.persistent = true;
                if (*maxAge <= 0) cookie.expiresAt = nowUnixSeconds;
                else {
                    const auto age = static_cast<std::uint64_t>(*maxAge);
                    cookie.expiresAt = age > std::numeric_limits<std::uint64_t>::max()
                            - nowUnixSeconds
                        ? std::numeric_limits<std::uint64_t>::max()
                        : nowUnixSeconds + age;
                }
            } else if (expires.has_value()) {
                cookie.persistent = true;
                cookie.expiresAt = *expires;
            } else {
                cookie.expiresAt = kSessionCookie;
            }

            const auto existing = std::find_if(m_impl->cookies.begin(), m_impl->cookies.end(),
                [&](const HttpCookie& current) {
                    return current.name == cookie.name && current.domain == cookie.domain
                        && current.path == cookie.path;
                });
            if (cookie.persistent && cookie.expiresAt <= nowUnixSeconds) {
                if (existing != m_impl->cookies.end()) m_impl->cookies.erase(existing);
                return {};
            }
            if (existing != m_impl->cookies.end()) {
                cookie.creationSequence = existing->creationSequence;
                *existing = std::move(cookie);
                return {};
            }
            if (m_impl->cookies.size() >= m_impl->limits.maxCookies) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP cookie jar entry limit is exhausted");
            }
            const auto domainCount = std::ranges::count_if(m_impl->cookies,
                [&](const HttpCookie& current) { return current.domain == cookie.domain; });
            if (static_cast<std::size_t>(domainCount)
                >= m_impl->limits.maxCookiesPerDomain) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP cookie per-domain limit is exhausted");
            }
            cookie.creationSequence = m_impl->nextSequence++;
            m_impl->cookies.push_back(std::move(cookie));
            return {};
        }

        Result<std::string> HttpCookieJar::CookieHeader(
            std::string_view requestUri,
            std::uint64_t nowUnixSeconds,
            const HttpCookieRequestContext& context) const {
            if (!m_impl) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP cookie jar is moved from");
            }
            const auto uri = ParseHttpUri(requestUri);
            if (!uri.IsOk()) return uri.PropagateFailure<std::string>();
            const auto query = uri.Value().pathAndQuery.find('?');
            const auto requestPath = uri.Value().pathAndQuery.substr(0, query);
            std::vector<const HttpCookie*> selected;
            for (const auto& cookie : m_impl->cookies) {
                if (cookie.persistent && cookie.expiresAt <= nowUnixSeconds) continue;
                if (cookie.secure && !uri.Value().Secure()) continue;
                if (cookie.hostOnly ? cookie.domain != uri.Value().host
                                    : !DomainMatch(uri.Value().host, cookie.domain)) {
                    continue;
                }
                if (!PathMatch(requestPath, cookie.path)) continue;
                const bool lax = cookie.sameSite == HttpCookieSameSite::Lax
                    || cookie.sameSite == HttpCookieSameSite::Unspecified;
                if (cookie.sameSite == HttpCookieSameSite::Strict && !context.sameSite) {
                    continue;
                }
                if (lax && !context.sameSite
                    && !(context.topLevelNavigation && context.safeMethod)) {
                    continue;
                }
                selected.push_back(&cookie);
            }
            std::ranges::sort(selected, [](const HttpCookie* left, const HttpCookie* right) {
                if (left->path.size() != right->path.size()) {
                    return left->path.size() > right->path.size();
                }
                return left->creationSequence < right->creationSequence;
            });
            std::string result;
            for (const auto* cookie : selected) {
                const std::size_t separatorBytes = result.empty() ? 0 : 2;
                const std::size_t cookieBytes = cookie->name.size() + 1 + cookie->value.size();
                if (separatorBytes > m_impl->limits.maxHeaderBytes - result.size()
                    || cookieBytes > m_impl->limits.maxHeaderBytes
                        - result.size() - separatorBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Cookie request header exceeds its configured limit");
                }
                if (!result.empty()) result.append("; ");
                result.append(cookie->name);
                result.push_back('=');
                result.append(cookie->value);
            }
            return result;
        }

        Result<void> HttpCookieJar::ClearExpired(std::uint64_t nowUnixSeconds) {
            if (!m_impl) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP cookie jar is moved from");
            }
            std::erase_if(m_impl->cookies, [&](const HttpCookie& cookie) {
                return cookie.persistent && cookie.expiresAt <= nowUnixSeconds;
            });
            return {};
        }

        HttpCookieJarSnapshot HttpCookieJar::Snapshot(
            std::uint64_t nowUnixSeconds) const noexcept {
            HttpCookieJarSnapshot snapshot;
            if (!m_impl) return snapshot;
            snapshot.cookies = m_impl->cookies.size();
            for (const auto& cookie : m_impl->cookies) {
                snapshot.persistentCookies += cookie.persistent ? 1 : 0;
                snapshot.secureCookies += cookie.secure ? 1 : 0;
                snapshot.expiredCookies += cookie.persistent
                        && cookie.expiresAt <= nowUnixSeconds
                    ? 1 : 0;
                snapshot.storedBytes += CookieStoredBytes(cookie);
            }
            return snapshot;
        }

        HttpCookieJarLimits HttpCookieJar::Limits() const noexcept {
            return m_impl ? m_impl->limits : HttpCookieJarLimits{};
        }

        void HttpCookieJar::Reset() noexcept {
            if (!m_impl) return;
            m_impl->cookies.clear();
        }

        Result<std::vector<HttpByteRangeSpec>> ParseHttpByteRanges(
            std::string_view value,
            std::size_t maxRanges) {
            value = Trim(value);
            if (maxRanges == 0 || value.size() < 6
                || !EqualIgnoreCase(value.substr(0, 6), "bytes=")) {
                return Status::InvalidArgument(u"HTTP Range must use bytes unit");
            }
            value.remove_prefix(6);
            std::vector<HttpByteRangeSpec> result;
            std::size_t cursor = 0;
            while (cursor <= value.size()) {
                const auto comma = value.find(',', cursor);
                const auto item = Trim(value.substr(cursor,
                    comma == std::string_view::npos ? value.size() - cursor : comma - cursor));
                if (item.empty() || result.size() >= maxRanges) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP Range count is invalid or exceeds its limit");
                }
                const auto dash = item.find('-');
                if (dash == std::string_view::npos || item.find('-', dash + 1)
                    != std::string_view::npos) {
                    return Status::InvalidArgument(u"HTTP byte range is malformed");
                }
                HttpByteRangeSpec spec;
                const auto first = Trim(item.substr(0, dash));
                const auto last = Trim(item.substr(dash + 1));
                if (first.empty() && last.empty()) {
                    return Status::InvalidArgument(u"HTTP byte range is empty");
                }
                if (!first.empty()) {
                    if (!ParseUnsigned(first, spec.first)) {
                        return Status::InvalidArgument(u"HTTP byte range start is invalid");
                    }
                    spec.hasFirst = true;
                }
                if (!last.empty()) {
                    if (!ParseUnsigned(last, spec.last)) {
                        return Status::InvalidArgument(u"HTTP byte range end is invalid");
                    }
                    spec.hasLast = true;
                }
                if (spec.hasFirst && spec.hasLast && spec.first > spec.last) {
                    return Status::InvalidArgument(u"HTTP byte range is descending");
                }
                result.push_back(spec);
                if (comma == std::string_view::npos) break;
                cursor = comma + 1;
            }
            return result;
        }

        Result<std::vector<HttpResolvedByteRange>> ResolveHttpByteRanges(
            std::span<const HttpByteRangeSpec> ranges,
            std::uint64_t representationLength) {
            if (ranges.empty()) {
                return Status::InvalidArgument(u"HTTP byte range list is empty");
            }
            std::vector<HttpResolvedByteRange> result;
            if (representationLength == 0) {
                return Status::NotFound(u"HTTP byte ranges are unsatisfied");
            }
            for (const auto& spec : ranges) {
                std::uint64_t first = 0;
                std::uint64_t last = representationLength - 1;
                if (!spec.hasFirst) {
                    if (!spec.hasLast || spec.last == 0) continue;
                    first = spec.last >= representationLength
                        ? 0 : representationLength - spec.last;
                } else {
                    if (spec.first >= representationLength) continue;
                    first = spec.first;
                    if (spec.hasLast) last = std::min(spec.last, representationLength - 1);
                }
                result.push_back({ first, last, last - first + 1 });
            }
            if (result.empty()) {
                return Status::NotFound(u"HTTP byte ranges are unsatisfied");
            }
            return result;
        }

        std::string BuildHttpContentRange(
            const HttpResolvedByteRange& range,
            std::uint64_t representationLength) {
            if (range.first > range.last || range.last >= representationLength
                || range.length != range.last - range.first + 1) {
                return {};
            }
            return "bytes " + std::to_string(range.first) + "-"
                + std::to_string(range.last) + "/"
                + std::to_string(representationLength);
        }

        std::string BuildHttpUnsatisfiedContentRange(
            std::uint64_t representationLength) {
            return "bytes */" + std::to_string(representationLength);
        }

        Result<HttpResponse> DecodeHttpContent(
            const HttpResponse& response,
            HttpContentDecoder& decoder,
            const HttpContentDecodingLimits& limits) {
            if (limits.maxCodings == 0 || limits.maxDecodedBytes == 0) {
                return Status::InvalidArgument(
                    u"HTTP content decoding limits must be non-zero");
            }
            std::vector<std::string> codings;
            for (const auto value : HeaderValues(response.headers, "content-encoding")) {
                std::size_t cursor = 0;
                while (cursor <= value.size()) {
                    const auto comma = value.find(',', cursor);
                    const auto coding = Trim(value.substr(cursor,
                        comma == std::string_view::npos ? value.size() - cursor : comma - cursor));
                    if (coding.empty()) {
                        return Status::InvalidArgument(u"Content-Encoding contains an empty coding");
                    }
                    if (!EqualIgnoreCase(coding, "identity")) codings.push_back(Lower(coding));
                    if (codings.size() > limits.maxCodings) {
                        return Status(StatusCode::ResourceExhausted,
                            u"Content-Encoding count exceeds its limit");
                    }
                    if (comma == std::string_view::npos) break;
                    cursor = comma + 1;
                }
            }
            HttpResponse result = response;
            std::vector<std::uint8_t> decoded = response.body;
            if (decoded.size() > limits.maxDecodedBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP content exceeds the decoded-byte limit");
            }
            for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
                const auto next = decoder.Decode(*coding, decoded, limits.maxDecodedBytes);
                if (!next.IsOk()) return next.PropagateFailure<HttpResponse>();
                decoded = next.Value();
                if (decoded.size() > limits.maxDecodedBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Decoded HTTP content exceeds its limit");
                }
            }
            result.body = std::move(decoded);
            const std::array<std::string_view, 2> normalized{
                "content-encoding", "content-length"
            };
            RemoveHeaders(result.headers, normalized);
            result.headers.push_back({ "Content-Length", std::to_string(result.body.size()) });
            return result;
        }

        namespace {
            bool IsHttpToken(std::string_view value) noexcept {
                if (value.empty()) return false;
                constexpr std::string_view special = "!#$%&'*+-.^_`|~";
                for (const unsigned char byte : value) {
                    if (std::isalnum(byte) == 0
                        && special.find(static_cast<char>(byte)) == std::string_view::npos) {
                        return false;
                    }
                }
                return true;
            }

            bool ValidMultipartBoundary(
                std::string_view boundary,
                std::size_t maxBytes) noexcept {
                if (boundary.empty() || maxBytes == 0 || boundary.size() > maxBytes
                    || boundary.back() == ' ') {
                    return false;
                }
                constexpr std::string_view punctuation = "'()+_,-./:=? ";
                for (const unsigned char byte : boundary) {
                    if (std::isalnum(byte) == 0
                        && punctuation.find(static_cast<char>(byte)) == std::string_view::npos) {
                        return false;
                    }
                }
                return true;
            }

            bool SplitParameter(
                std::string_view segment,
                std::string& name,
                std::string& value) {
                const auto equals = segment.find('=');
                if (equals == std::string_view::npos) return false;
                const auto nameView = Trim(segment.substr(0, equals));
                auto valueView = Trim(segment.substr(equals + 1));
                if (!IsHttpToken(nameView) || valueView.empty()) return false;
                name = Lower(nameView);
                value.clear();
                if (valueView.front() != '"') {
                    if (!IsHttpToken(valueView)) return false;
                    value.assign(valueView);
                    return true;
                }
                if (valueView.size() < 2 || valueView.back() != '"') return false;
                valueView.remove_prefix(1);
                valueView.remove_suffix(1);
                bool escaped = false;
                for (const unsigned char byte : valueView) {
                    if (escaped) {
                        if (byte == '\r' || byte == '\n') return false;
                        value.push_back(static_cast<char>(byte));
                        escaped = false;
                    } else if (byte == '\\') {
                        escaped = true;
                    } else {
                        if (byte == '\r' || byte == '\n') return false;
                        value.push_back(static_cast<char>(byte));
                    }
                }
                return !escaped;
            }

            Result<void> ExtractMultipartDisposition(HttpMultipartPart& part) {
                const auto values = HeaderValues(part.headers, "content-disposition");
                if (values.empty()) return {};
                if (values.size() != 1) {
                    return Status::InvalidArgument(
                        u"Multipart part has duplicate Content-Disposition");
                }
                const auto value = values.front();
                const auto semicolon = value.find(';');
                if (!EqualIgnoreCase(Trim(value.substr(0, semicolon)), "form-data")) return {};
                std::size_t cursor = semicolon;
                while (cursor != std::string_view::npos && cursor < value.size()) {
                    ++cursor;
                    const auto next = value.find(';', cursor);
                    const auto segment = Trim(value.substr(cursor,
                        next == std::string_view::npos ? value.size() - cursor : next - cursor));
                    std::string name;
                    std::string parameterValue;
                    if (!SplitParameter(segment, name, parameterValue)) {
                        return Status::InvalidArgument(
                            u"Multipart Content-Disposition parameter is invalid");
                    }
                    if (name == "name") part.name = std::move(parameterValue);
                    else if (name == "filename") part.filename = std::move(parameterValue);
                    cursor = next;
                }
                return {};
            }

            bool AppendBounded(
                std::vector<std::uint8_t>& output,
                std::string_view value,
                std::size_t limit) {
                if (output.size() > limit || value.size() > limit - output.size()) return false;
                output.insert(output.end(), value.begin(), value.end());
                return true;
            }

            bool AppendBounded(
                std::vector<std::uint8_t>& output,
                std::span<const std::uint8_t> value,
                std::size_t limit) {
                if (output.size() > limit || value.size() > limit - output.size()) return false;
                output.insert(output.end(), value.begin(), value.end());
                return true;
            }

            std::vector<std::string_view> SplitOutsideQuotes(std::string_view value) {
                std::vector<std::string_view> segments;
                bool quoted = false;
                bool escaped = false;
                std::size_t begin = 0;
                for (std::size_t index = 0; index < value.size(); ++index) {
                    const char byte = value[index];
                    if (escaped) {
                        escaped = false;
                    } else if (quoted && byte == '\\') {
                        escaped = true;
                    } else if (byte == '"') {
                        quoted = !quoted;
                    } else if (!quoted && byte == ',') {
                        segments.push_back(Trim(value.substr(begin, index - begin)));
                        begin = index + 1;
                    }
                }
                if (quoted || escaped) return {};
                segments.push_back(Trim(value.substr(begin)));
                return segments;
            }

            bool ValidHeaderValue(std::string_view value) noexcept {
                for (const unsigned char byte : value) {
                    if (byte == '\r' || byte == '\n' || byte == 0x7F
                        || (byte < 0x20 && byte != '\t')) {
                        return false;
                    }
                }
                return true;
            }

            std::string Base64(std::string_view value) {
                static constexpr std::string_view alphabet =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string result;
                result.reserve(((value.size() + 2) / 3) * 4);
                for (std::size_t index = 0; index < value.size(); index += 3) {
                    const auto first = static_cast<unsigned char>(value[index]);
                    const auto second = index + 1 < value.size()
                        ? static_cast<unsigned char>(value[index + 1]) : 0;
                    const auto third = index + 2 < value.size()
                        ? static_cast<unsigned char>(value[index + 2]) : 0;
                    const auto combined = (static_cast<std::uint32_t>(first) << 16)
                        | (static_cast<std::uint32_t>(second) << 8)
                        | static_cast<std::uint32_t>(third);
                    result.push_back(alphabet[(combined >> 18) & 0x3F]);
                    result.push_back(alphabet[(combined >> 12) & 0x3F]);
                    result.push_back(index + 1 < value.size()
                        ? alphabet[(combined >> 6) & 0x3F] : '=');
                    result.push_back(index + 2 < value.size()
                        ? alphabet[combined & 0x3F] : '=');
                }
                return result;
            }

            bool DefaultCacheableStatus(int status) noexcept {
                switch (status) {
                case 200: case 203: case 204: case 206: case 300: case 301:
                case 308: case 404: case 405: case 410: case 414: case 501:
                    return true;
                default:
                    return false;
                }
            }

            Result<void> SetNumericDirective(
                std::optional<std::uint64_t>& target,
                std::string_view value) {
                value = Trim(value);
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value.remove_prefix(1);
                    value.remove_suffix(1);
                }
                std::uint64_t parsed = 0;
                if (!ParseUnsigned(value, parsed)) {
                    return Status::InvalidArgument(
                        u"Cache-Control numeric directive is invalid");
                }
                if (target.has_value() && *target != parsed) {
                    return Status::InvalidArgument(
                        u"Cache-Control has conflicting numeric directives");
                }
                target = parsed;
                return {};
            }
        }

        Result<std::string> ParseHttpMultipartBoundary(
            std::string_view contentType,
            std::size_t maxBoundaryBytes) {
            const auto semicolon = contentType.find(';');
            const auto mediaType = Trim(contentType.substr(0, semicolon));
            if (!Lower(mediaType).starts_with("multipart/")) {
                return Status::InvalidArgument(u"Content-Type is not multipart");
            }
            std::optional<std::string> boundary;
            std::size_t cursor = semicolon;
            while (cursor != std::string_view::npos && cursor < contentType.size()) {
                ++cursor;
                const auto next = contentType.find(';', cursor);
                const auto segment = Trim(contentType.substr(cursor,
                    next == std::string_view::npos
                        ? contentType.size() - cursor : next - cursor));
                std::string name;
                std::string value;
                if (!SplitParameter(segment, name, value)) {
                    return Status::InvalidArgument(u"Multipart Content-Type parameter is invalid");
                }
                if (name == "boundary") {
                    if (boundary.has_value()) {
                        return Status::InvalidArgument(u"Multipart boundary is duplicated");
                    }
                    boundary = std::move(value);
                }
                cursor = next;
            }
            if (!boundary.has_value()
                || !ValidMultipartBoundary(*boundary, maxBoundaryBytes)) {
                return Status::InvalidArgument(u"Multipart boundary is missing or invalid");
            }
            return *boundary;
        }

        Result<std::vector<HttpMultipartPart>> ParseHttpMultipart(
            std::string_view contentType,
            std::span<const std::uint8_t> body,
            const HttpMultipartLimits& limits) {
            if (limits.maxParts == 0 || limits.maxHeaderBytesPerPart == 0
                || limits.maxBodyBytesPerPart == 0 || limits.maxTotalBytes == 0
                || body.size() > limits.maxTotalBytes) {
                return Status(StatusCode::ResourceExhausted,
                    u"Multipart limits are invalid or total body is too large");
            }
            const auto boundary = ParseHttpMultipartBoundary(
                contentType, limits.maxBoundaryBytes);
            if (!boundary.IsOk()) {
                return boundary.PropagateFailure<std::vector<HttpMultipartPart>>();
            }
            const std::string delimiter = "--" + boundary.Value();
            const std::string marker = "\r\n" + delimiter;
            const std::string_view wire(
                reinterpret_cast<const char*>(body.data()), body.size());
            std::size_t cursor = wire.find(delimiter);
            if (cursor == std::string_view::npos
                || (cursor != 0 && (cursor < 2 || wire.substr(cursor - 2, 2) != "\r\n"))) {
                return Status::InvalidArgument(u"Multipart opening boundary is missing");
            }
            cursor += delimiter.size();
            std::vector<HttpMultipartPart> parts;
            while (true) {
                if (wire.substr(cursor, 2) == "--") {
                    cursor += 2;
                    while (cursor < wire.size()
                        && (wire[cursor] == ' ' || wire[cursor] == '\t')) {
                        ++cursor;
                    }
                    if (cursor != wire.size() && wire.substr(cursor, 2) != "\r\n") {
                        return Status::InvalidArgument(
                            u"Multipart closing boundary has an invalid suffix");
                    }
                    break;
                }
                if (wire.substr(cursor, 2) != "\r\n") {
                    return Status::InvalidArgument(u"Multipart boundary line is malformed");
                }
                cursor += 2;
                const auto headerEnd = wire.find("\r\n\r\n", cursor);
                if (headerEnd == std::string_view::npos
                    || headerEnd - cursor > limits.maxHeaderBytesPerPart) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart part headers are missing or too large");
                }
                if (parts.size() >= limits.maxParts) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart part count exceeds its limit");
                }
                HttpMultipartPart part;
                std::size_t lineBegin = cursor;
                while (lineBegin < headerEnd) {
                    const auto lineEnd = wire.find("\r\n", lineBegin);
                    const auto actualEnd = lineEnd == std::string_view::npos
                        || lineEnd > headerEnd ? headerEnd : lineEnd;
                    const auto line = wire.substr(lineBegin, actualEnd - lineBegin);
                    const auto colon = line.find(':');
                    if (colon == std::string_view::npos) {
                        return Status::InvalidArgument(u"Multipart part header is malformed");
                    }
                    const auto name = Trim(line.substr(0, colon));
                    const auto value = Trim(line.substr(colon + 1));
                    if (!IsHttpToken(name) || !ValidHeaderValue(value)) {
                        return Status::InvalidArgument(u"Multipart part header is invalid");
                    }
                    part.headers.push_back({ std::string(name), std::string(value) });
                    lineBegin = actualEnd + 2;
                }
                const auto disposition = ExtractMultipartDisposition(part);
                if (!disposition.IsOk()) {
                    return Result<std::vector<HttpMultipartPart>>(disposition.GetStatus());
                }
                const auto contentBegin = headerEnd + 4;
                const auto nextBoundary = wire.find(marker, contentBegin);
                if (nextBoundary == std::string_view::npos) {
                    return Status::InvalidArgument(u"Multipart closing boundary is missing");
                }
                const auto bodyBytes = nextBoundary - contentBegin;
                if (bodyBytes > limits.maxBodyBytesPerPart) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart part body exceeds its limit");
                }
                part.body.assign(body.begin() + static_cast<std::ptrdiff_t>(contentBegin),
                    body.begin() + static_cast<std::ptrdiff_t>(nextBoundary));
                parts.push_back(std::move(part));
                cursor = nextBoundary + marker.size();
            }
            if (wire.substr(cursor, 2) == "\r\n") cursor += 2;
            if (cursor < wire.size()) {
                // Epilogue is permitted but still covered by maxTotalBytes.
            }
            return parts;
        }

        Result<std::vector<std::uint8_t>> BuildHttpMultipart(
            std::string_view boundary,
            std::span<const HttpMultipartPart> parts,
            const HttpMultipartLimits& limits) {
            if (!ValidMultipartBoundary(boundary, limits.maxBoundaryBytes)
                || limits.maxParts == 0 || parts.size() > limits.maxParts
                || limits.maxTotalBytes == 0) {
                return Status::InvalidArgument(u"Multipart build limits or boundary are invalid");
            }
            std::vector<std::uint8_t> output;
            const std::string delimiter = "--" + std::string(boundary);
            const std::string collision = "\r\n" + delimiter;
            for (const auto& part : parts) {
                std::size_t headerBytes = 0;
                if (part.body.size() > limits.maxBodyBytesPerPart
                    || std::search(part.body.begin(), part.body.end(),
                        collision.begin(), collision.end()) != part.body.end()) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart part body is too large or contains its boundary");
                }
                if (!AppendBounded(output, delimiter + "\r\n", limits.maxTotalBytes)) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart output exceeds its total limit");
                }
                for (const auto& header : part.headers) {
                    if (!IsHttpToken(header.name) || !ValidHeaderValue(header.value)) {
                        return Status::InvalidArgument(u"Multipart output header is invalid");
                    }
                    const std::size_t lineBytes = header.name.size() + header.value.size() + 4;
                    if (lineBytes > limits.maxHeaderBytesPerPart - headerBytes) {
                        return Status(StatusCode::ResourceExhausted,
                            u"Multipart output headers exceed their limit");
                    }
                    headerBytes += lineBytes;
                    if (!AppendBounded(output,
                            header.name + ": " + header.value + "\r\n",
                            limits.maxTotalBytes)) {
                        return Status(StatusCode::ResourceExhausted,
                            u"Multipart output exceeds its total limit");
                    }
                }
                if (!AppendBounded(output, "\r\n", limits.maxTotalBytes)
                    || !AppendBounded(output, part.body, limits.maxTotalBytes)
                    || !AppendBounded(output, "\r\n", limits.maxTotalBytes)) {
                    return Status(StatusCode::ResourceExhausted,
                        u"Multipart output exceeds its total limit");
                }
            }
            if (!AppendBounded(output, delimiter + "--\r\n", limits.maxTotalBytes)) {
                return Status(StatusCode::ResourceExhausted,
                    u"Multipart closing boundary exceeds the total limit");
            }
            return output;
        }

        Result<std::vector<HttpAuthenticationChallenge>>
            ParseHttpAuthenticationChallenges(
                std::string_view value,
                std::size_t maxChallenges,
                std::size_t maxBytes) {
            if (maxChallenges == 0 || maxBytes == 0 || value.empty()
                || value.size() > maxBytes || !ValidHeaderValue(value)) {
                return Status(StatusCode::ResourceExhausted,
                    u"Authentication challenge input is empty or exceeds its limit");
            }
            const auto segments = SplitOutsideQuotes(value);
            if (segments.empty()) {
                return Status::InvalidArgument(u"Authentication challenge quoting is invalid");
            }
            std::vector<HttpAuthenticationChallenge> result;
            for (const auto segment : segments) {
                if (segment.empty()) {
                    return Status::InvalidArgument(u"Authentication challenge segment is empty");
                }
                std::size_t tokenEnd = 0;
                while (tokenEnd < segment.size()) {
                    const unsigned char byte = static_cast<unsigned char>(segment[tokenEnd]);
                    if (std::isalnum(byte) == 0
                        && std::string_view("!#$%&'*+-.^_`|~").find(
                            static_cast<char>(byte)) == std::string_view::npos) {
                        break;
                    }
                    ++tokenEnd;
                }
                const bool newChallenge = tokenEnd != 0
                    && (tokenEnd == segment.size()
                        || segment[tokenEnd] == ' ' || segment[tokenEnd] == '\t');
                if (newChallenge) {
                    if (result.size() >= maxChallenges) {
                        return Status(StatusCode::ResourceExhausted,
                            u"Authentication challenge count exceeds its limit");
                    }
                    auto credentials = Trim(segment.substr(tokenEnd));
                    result.push_back({ Lower(segment.substr(0, tokenEnd)),
                        std::string(credentials) });
                } else {
                    if (result.empty() || segment.find('=') == std::string_view::npos) {
                        return Status::InvalidArgument(
                            u"Authentication challenge continuation is invalid");
                    }
                    if (!result.back().credentials.empty()) result.back().credentials.append(", ");
                    result.back().credentials.append(segment);
                }
            }
            return result;
        }

        Result<std::string> BuildHttpBasicAuthorization(
            std::string_view user,
            std::string_view password,
            std::size_t maxCredentialBytes) {
            if (maxCredentialBytes == 0 || user.size() > maxCredentialBytes
                || password.size() > maxCredentialBytes - user.size()
                || user.find(':') != std::string_view::npos
                || !ValidHeaderValue(user) || !ValidHeaderValue(password)) {
                return Status::InvalidArgument(u"Basic authentication credentials are invalid");
            }
            std::string credentials(user);
            credentials.push_back(':');
            credentials.append(password);
            return "Basic " + Base64(credentials);
        }

        Result<std::string> BuildHttpBearerAuthorization(
            std::string_view token,
            std::size_t maxTokenBytes) {
            if (maxTokenBytes == 0 || token.empty() || token.size() > maxTokenBytes
                || !ValidHeaderValue(token)) {
                return Status::InvalidArgument(u"Bearer token is empty or invalid");
            }
            bool padding = false;
            for (const unsigned char byte : token) {
                if (byte == '=') {
                    padding = true;
                    continue;
                }
                const bool valid = std::isalnum(byte) != 0 || byte == '-' || byte == '.'
                    || byte == '_' || byte == '~' || byte == '+' || byte == '/';
                if (!valid || padding) {
                    return Status::InvalidArgument(u"Bearer token syntax is invalid");
                }
            }
            return "Bearer " + std::string(token);
        }

        Result<HttpCacheControl> ParseHttpCacheControl(
            std::span<const HttpHeader> headers,
            std::size_t maxDirectives) {
            if (maxDirectives == 0) {
                return Status::InvalidArgument(u"Cache-Control directive limit must be non-zero");
            }
            HttpCacheControl result;
            std::size_t directives = 0;
            for (const auto& header : headers) {
                if (!HeaderNameIs(header, "cache-control")) continue;
                const auto segments = SplitOutsideQuotes(header.value);
                if (segments.empty()) {
                    return Status::InvalidArgument(u"Cache-Control quoting is invalid");
                }
                for (const auto segment : segments) {
                    if (segment.empty() || ++directives > maxDirectives) {
                        return Status(StatusCode::ResourceExhausted,
                            u"Cache-Control directives exceed their limit");
                    }
                    const auto equals = segment.find('=');
                    const auto name = Lower(Trim(segment.substr(0, equals)));
                    const auto value = equals == std::string_view::npos
                        ? std::string_view{} : Trim(segment.substr(equals + 1));
                    if (!IsHttpToken(name)) {
                        return Status::InvalidArgument(u"Cache-Control directive name is invalid");
                    }
                    if (name == "no-store") result.noStore = true;
                    else if (name == "no-cache") result.noCache = true;
                    else if (name == "must-revalidate") result.mustRevalidate = true;
                    else if (name == "proxy-revalidate") result.proxyRevalidate = true;
                    else if (name == "private") result.isPrivate = true;
                    else if (name == "public") result.isPublic = true;
                    else if (name == "immutable") result.immutable = true;
                    else if (name == "max-age") {
                        const auto status = SetNumericDirective(result.maxAge, value);
                        if (!status.IsOk()) return Result<HttpCacheControl>(status.GetStatus());
                    } else if (name == "s-maxage") {
                        const auto status = SetNumericDirective(result.sharedMaxAge, value);
                        if (!status.IsOk()) return Result<HttpCacheControl>(status.GetStatus());
                    } else if (name == "stale-while-revalidate") {
                        const auto status = SetNumericDirective(
                            result.staleWhileRevalidate, value);
                        if (!status.IsOk()) return Result<HttpCacheControl>(status.GetStatus());
                    } else if (name == "stale-if-error") {
                        const auto status = SetNumericDirective(result.staleIfError, value);
                        if (!status.IsOk()) return Result<HttpCacheControl>(status.GetStatus());
                    }
                }
            }
            return result;
        }

        Result<void> ApplyHttpCacheValidators(
            HttpRequest& request,
            const HttpCacheValidators& validators) {
            if ((!validators.etag.empty() && !ValidHeaderValue(validators.etag))
                || (!validators.lastModified.empty()
                    && !ValidHeaderValue(validators.lastModified))) {
                return Status::InvalidArgument(u"HTTP cache validator is invalid");
            }
            if (!validators.etag.empty()) {
                SetHeader(request.headers, "If-None-Match", validators.etag);
            }
            if (!validators.lastModified.empty()) {
                SetHeader(request.headers, "If-Modified-Since", validators.lastModified);
            }
            return {};
        }

        Result<HttpCacheEvaluation> EvaluateHttpCacheResponse(
            const HttpRequest& request,
            const HttpResponse& response,
            bool sharedCache) {
            const auto requestControl = ParseHttpCacheControl(request.headers);
            if (!requestControl.IsOk()) {
                return requestControl.PropagateFailure<HttpCacheEvaluation>();
            }
            const auto responseControl = ParseHttpCacheControl(response.headers);
            if (!responseControl.IsOk()) {
                return responseControl.PropagateFailure<HttpCacheEvaluation>();
            }
            HttpCacheEvaluation result;
            if (!(EqualIgnoreCase(request.method, "GET")
                    || EqualIgnoreCase(request.method, "HEAD"))
                || !DefaultCacheableStatus(response.statusCode)
                || requestControl.Value().noStore || responseControl.Value().noStore
                || (sharedCache && responseControl.Value().isPrivate)) {
                return result;
            }
            const auto vary = HeaderValues(response.headers, "vary");
            for (const auto value : vary) {
                const auto varySegments = SplitOutsideQuotes(value);
                if (varySegments.empty() && !value.empty()) {
                    return Status::InvalidArgument(u"Vary header quoting is invalid");
                }
                for (const auto segment : varySegments) {
                    if (Trim(segment) == "*") return result;
                }
            }
            const bool authorized = !HeaderValues(request.headers, "authorization").empty();
            if (sharedCache && authorized && !responseControl.Value().isPublic
                && !responseControl.Value().sharedMaxAge.has_value()
                && !responseControl.Value().mustRevalidate
                && !responseControl.Value().proxyRevalidate) {
                return result;
            }
            result.cacheable = true;
            result.requiresRevalidation = requestControl.Value().noCache
                || responseControl.Value().noCache
                || responseControl.Value().mustRevalidate;
            result.freshnessLifetime = sharedCache
                && responseControl.Value().sharedMaxAge.has_value()
                ? responseControl.Value().sharedMaxAge
                : responseControl.Value().maxAge;
            return result;
        }
    }
}
