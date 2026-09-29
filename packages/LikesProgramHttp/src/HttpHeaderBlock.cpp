#include <LikesProgram/Http/HttpHeaderBlock.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace {
    using LikesProgram::Http::HttpHeader;

    bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto lower = [](char value) noexcept {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a') : value;
            };
            if (lower(left[index]) != lower(right[index])) return false;
        }
        return true;
    }

    bool IsTokenCharacter(unsigned char byte) noexcept {
        if ((byte >= '0' && byte <= '9')
            || (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z')) return true;
        switch (byte) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
        }
    }

    bool IsLowercaseToken(std::string_view value) noexcept {
        if (value.empty()) return false;
        return std::all_of(value.begin(), value.end(), [](char byte) {
            return IsTokenCharacter(static_cast<unsigned char>(byte))
                && !(byte >= 'A' && byte <= 'Z');
        });
    }

    bool IsSafeValue(std::string_view value) noexcept {
        if (!value.empty()
            && (value.front() == ' ' || value.front() == '\t'
                || value.back() == ' ' || value.back() == '\t')) {
            return false;
        }
        for (const unsigned char byte : value) {
            if (byte == '\r' || byte == '\n' || byte == 0
                || (byte < 0x20 && byte != '\t') || byte == 0x7F) {
                return false;
            }
        }
        return true;
    }

    bool IsForbiddenConnectionField(std::string_view name) noexcept {
        return EqualsIgnoreCase(name, "connection")
            || EqualsIgnoreCase(name, "keep-alive")
            || EqualsIgnoreCase(name, "proxy-connection")
            || EqualsIgnoreCase(name, "transfer-encoding")
            || EqualsIgnoreCase(name, "upgrade");
    }

    std::string TrimLower(std::string_view value) {
        std::size_t begin = 0;
        while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
        std::size_t end = value.size();
        while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
        std::string result(value.substr(begin, end - begin));
        for (char& byte : result) {
            if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
        }
        return result;
    }

    bool HasPseudo(
        const std::vector<HttpHeader>& headers,
        std::string_view name) noexcept {
        for (const auto& header : headers) {
            if (header.name == name) return true;
        }
        return false;
    }

    std::string HeaderValue(
        const std::vector<HttpHeader>& headers,
        std::string_view name) {
        for (const auto& header : headers) {
            if (header.name == name) return header.value;
        }
        return {};
    }
}

namespace LikesProgram {
    namespace Http {
        Result<void> ValidateHttpHeaderBlock(
            const std::vector<HttpHeader>& headers,
            HttpHeaderBlockProtocol,
            HttpHeaderBlockOptions options) {
            bool regularSeen = false;
            bool hasMethod = false;
            bool hasStatus = false;
            std::vector<std::string_view> pseudoNames;
            for (const auto& header : headers) {
                if (header.name.empty()) {
                    return Status::InvalidArgument(u"HTTP/2/3 header name must not be empty");
                }
                if (!IsLowercaseToken(header.name)
                    && !(header.name.size() > 1 && header.name.front() == ':'
                        && IsLowercaseToken(std::string_view(header.name).substr(1)))) {
                    return Status::InvalidArgument(
                        u"HTTP/2/3 header names must be lowercase tokens");
                }
                if (!IsSafeValue(header.value)) {
                    return Status::InvalidArgument(u"HTTP/2/3 header value contains control bytes");
                }
                if (header.name.front() == ':') {
                    if (options.isTrailer || regularSeen) {
                        return Status::InvalidArgument(
                            u"HTTP/2/3 pseudo-headers must precede regular headers");
                    }
                    if (std::find(pseudoNames.begin(), pseudoNames.end(), header.name)
                        != pseudoNames.end()) {
                        return Status::InvalidArgument(u"duplicate HTTP/2/3 pseudo-header");
                    }
                    pseudoNames.push_back(header.name);
                    if (header.name == ":method") {
                        hasMethod = true;
                    } else if (header.name == ":status") {
                        hasStatus = true;
                    } else if (header.name != ":scheme"
                        && header.name != ":authority"
                        && header.name != ":path"
                        && header.name != ":protocol") {
                        return Status::InvalidArgument(u"unknown HTTP/2/3 pseudo-header");
                    }
                    if (options.isRequest && header.name == ":status") {
                        return Status::InvalidArgument(u"request must not contain :status");
                    }
                    if (!options.isRequest && header.name != ":status") {
                        return Status::InvalidArgument(u"response contains request pseudo-header");
                    }
                    continue;
                }
                regularSeen = true;
                if (IsForbiddenConnectionField(header.name)) {
                    return Status::InvalidArgument(
                        u"connection-specific header is forbidden in HTTP/2/3");
                }
                if (header.name == "te" && TrimLower(header.value) != "trailers") {
                    return Status::InvalidArgument(u"HTTP/2/3 TE only permits trailers");
                }
            }
            if (options.isTrailer) {
                if (hasMethod || hasStatus) {
                    return Status::InvalidArgument(
                        u"HTTP/2/3 trailers must not contain pseudo-headers");
                }
                return {};
            }
            if (options.isRequest) {
                if (!hasMethod) return Status::InvalidArgument(u"HTTP/2/3 request lacks :method");
                const std::string method = HeaderValue(headers, ":method");
                if (method.empty()) return Status::InvalidArgument(u"HTTP/2/3 :method must not be empty");
                if (EqualsIgnoreCase(method, "CONNECT")) {
                    if (!HasPseudo(headers, ":authority")) {
                        return Status::InvalidArgument(u"CONNECT request lacks :authority");
                    }
                    if (HeaderValue(headers, ":authority").empty()) {
                        return Status::InvalidArgument(u"CONNECT request lacks :authority value");
                    }
                    if (HasPseudo(headers, ":protocol")
                        && (!HasPseudo(headers, ":scheme") || !HasPseudo(headers, ":path"))) {
                        return Status::InvalidArgument(
                            u"extended CONNECT requires :scheme and :path");
                    }
                    if (HasPseudo(headers, ":protocol")
                        && HeaderValue(headers, ":protocol").empty()) {
                        return Status::InvalidArgument(
                            u"extended CONNECT requires a :protocol value");
                    }
                    if (!HasPseudo(headers, ":protocol")
                        && (HasPseudo(headers, ":scheme") || HasPseudo(headers, ":path"))) {
                        return Status::InvalidArgument(
                            u"ordinary CONNECT must not carry :scheme or :path");
                    }
                } else if (HasPseudo(headers, ":protocol")) {
                    return Status::InvalidArgument(u":protocol requires CONNECT");
                } else if (!HasPseudo(headers, ":scheme") || !HasPseudo(headers, ":path")) {
                    return Status::InvalidArgument(
                        u"HTTP/2/3 request requires :scheme and :path");
                } else if (HeaderValue(headers, ":scheme").empty()
                    || HeaderValue(headers, ":path").empty()) {
                    return Status::InvalidArgument(
                        u"HTTP/2/3 request pseudo-header values must not be empty");
                }
            } else if (!hasStatus) {
                return Status::InvalidArgument(u"HTTP/2/3 response lacks :status");
            } else {
                const std::string status = HeaderValue(headers, ":status");
                if (status.size() != 3
                    || !std::all_of(status.begin(), status.end(), [](char byte) {
                        return byte >= '0' && byte <= '9';
                    })) {
                    return Status::InvalidArgument(u"HTTP/2/3 :status must be a three-digit code");
                }
            }
            if (HasPseudo(headers, ":authority") && HasPseudo(headers, ":status")) {
                return Status::InvalidArgument(u"response must not contain :authority");
            }
            return {};
        }
    }
}
