#include <LikesProgram/Http/Http1.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {
    using LikesProgram::Http::Http1MessageLimits;
    using LikesProgram::Http::HttpHeader;
    using LikesProgram::Http::Http1ConnectionState;
    using LikesProgram::Http::Http1MessageKind;
    using LikesProgram::StatusCode;

    bool IsOptionalWhitespace(char ch) noexcept {
        return ch == ' ' || ch == '\t';
    }

    bool IsHttpTokenCharacter(unsigned char byte) noexcept {
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

    unsigned char ToLowerAscii(unsigned char byte) noexcept {
        return byte >= 'A' && byte <= 'Z'
            ? static_cast<unsigned char>(byte + ('a' - 'A'))
            : byte;
    }

    bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (ToLowerAscii(static_cast<unsigned char>(lhs[i]))
                != ToLowerAscii(static_cast<unsigned char>(rhs[i]))) return false;
        }
        return true;
    }

    bool IsSafeToken(std::string_view value) noexcept {
        if (value.empty()) return false;
        for (char ch : value) {
            if (!IsHttpTokenCharacter(static_cast<unsigned char>(ch))) return false;
        }
        return true;
    }

    bool IsSafeFieldValue(std::string_view value) noexcept {
        for (char ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if ((byte < 0x20 && byte != '\t') || byte == 0x7F) return false;
        }
        return true;
    }

    std::string TrimHeaderValue(std::string_view value) {
        std::size_t begin = 0;
        while (begin < value.size() && IsOptionalWhitespace(value[begin])) ++begin;
        std::size_t end = value.size();
        while (end > begin && IsOptionalWhitespace(value[end - 1])) --end;
        return std::string(value.substr(begin, end - begin));
    }

    bool IsHttp1Version(std::string_view value) noexcept {
        return value == "HTTP/1.0" || value == "HTTP/1.1";
    }

    bool IsSafeRequestTarget(std::string_view value) noexcept {
        if (value.empty()) return false;
        for (char ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte <= 0x20 || byte == 0x7F) return false;
        }
        return true;
    }

    LikesProgram::Result<std::vector<HttpHeader>> ParseHeaderLines(std::string_view block) {
        std::vector<HttpHeader> headers;
        std::size_t lineBegin = 0;
        while (lineBegin < block.size()) {
            const std::size_t lineEnd = block.find("\r\n", lineBegin);
            const std::size_t actualEnd = lineEnd == std::string_view::npos
                ? block.size() : lineEnd;
            const std::string_view line = block.substr(lineBegin, actualEnd - lineBegin);
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                return LikesProgram::Status::InvalidArgument(u"malformed HTTP header line");
            }
            const std::string_view name = line.substr(0, colon);
            const std::string_view rawValue = line.substr(colon + 1);
            if (!IsSafeToken(name) || !IsSafeFieldValue(rawValue)) {
                return LikesProgram::Status::InvalidArgument(u"invalid HTTP header");
            }
            headers.push_back({ std::string(name), TrimHeaderValue(rawValue) });
            if (lineEnd == std::string_view::npos) break;
            lineBegin = lineEnd + 2;
        }
        return headers;
    }

    struct Framing {
        bool hasLength = false;
        std::size_t contentLength = 0;
        bool hasTransferEncoding = false;
        bool chunked = false;
    };

    LikesProgram::Result<std::size_t> ParseContentLength(std::string_view value) {
        const std::string text = TrimHeaderValue(value);
        if (text.empty() || text.find(',') != std::string::npos) {
            return LikesProgram::Status::InvalidArgument(u"invalid Content-Length");
        }
        std::size_t parsedValue = 0;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, parsedValue);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return LikesProgram::Status::InvalidArgument(u"invalid Content-Length");
        }
        return parsedValue;
    }

    bool IsValidChunkExtensions(std::string_view extensions) noexcept {
        std::size_t pos = 0;
        while (pos < extensions.size()) {
            if (extensions[pos] != ';') return false;
            ++pos;
            while (pos < extensions.size() && IsOptionalWhitespace(extensions[pos])) ++pos;
            const std::size_t nameBegin = pos;
            while (pos < extensions.size() && IsHttpTokenCharacter(
                static_cast<unsigned char>(extensions[pos]))) ++pos;
            if (nameBegin == pos) return false;
            while (pos < extensions.size() && IsOptionalWhitespace(extensions[pos])) ++pos;
            if (pos == extensions.size()) continue;
            if (extensions[pos] != '=') return false;
            ++pos;
            while (pos < extensions.size() && IsOptionalWhitespace(extensions[pos])) ++pos;
            const std::size_t valueBegin = pos;
            if (pos < extensions.size() && extensions[pos] == '"') {
                ++pos;
                bool escaped = false;
                while (pos < extensions.size()) {
                    const auto byte = static_cast<unsigned char>(extensions[pos++]);
                    if (escaped) {
                        if (byte != '\\' && byte != '"') return false;
                        escaped = false;
                    } else if (byte == '\\') {
                        escaped = true;
                    } else if (byte == '"') {
                        break;
                    } else if (byte < 0x20 || byte == 0x7F) {
                        return false;
                    }
                }
                if (escaped || pos > extensions.size() || extensions[pos - 1] != '"') return false;
            } else {
                while (pos < extensions.size() && IsHttpTokenCharacter(
                    static_cast<unsigned char>(extensions[pos]))) ++pos;
                if (valueBegin == pos) return false;
            }
            while (pos < extensions.size() && IsOptionalWhitespace(extensions[pos])) ++pos;
        }
        return true;
    }

    LikesProgram::Result<Framing> ReadFraming(
        const std::vector<HttpHeader>& headers) {
        Framing framing;
        for (const auto& header : headers) {
            if (EqualsIgnoreCase(header.name, "Content-Length")) {
                if (framing.hasLength) {
                    return LikesProgram::Status::InvalidArgument(u"duplicate Content-Length");
                }
                auto value = ParseContentLength(header.value);
                if (!value.IsOk()) return value.GetStatus();
                framing.hasLength = true;
                framing.contentLength = value.Value();
                continue;
            }
            if (!EqualsIgnoreCase(header.name, "Transfer-Encoding")) continue;
            if (framing.hasTransferEncoding) {
                return LikesProgram::Status::InvalidArgument(u"duplicate Transfer-Encoding");
            }
            framing.hasTransferEncoding = true;
            std::size_t begin = 0;
            std::size_t codingCount = 0;
            while (begin <= header.value.size()) {
                const std::size_t comma = header.value.find(',', begin);
                const std::size_t end = comma == std::string_view::npos
                    ? header.value.size() : comma;
                const std::string coding = TrimHeaderValue(
                    std::string_view(header.value).substr(begin, end - begin));
                if (!IsSafeToken(coding) || !EqualsIgnoreCase(coding, "chunked")) {
                    return LikesProgram::Status::InvalidArgument(
                        u"unsupported Transfer-Encoding");
                }
                ++codingCount;
                if (comma == std::string_view::npos) break;
                begin = comma + 1;
            }
            if (codingCount != 1) {
                return LikesProgram::Status::InvalidArgument(u"ambiguous Transfer-Encoding");
            }
            framing.chunked = true;
        }
        if (framing.hasTransferEncoding && framing.hasLength) {
            return LikesProgram::Status::InvalidArgument(
                u"Transfer-Encoding and Content-Length conflict");
        }
        return framing;
    }

    LikesProgram::Result<std::pair<std::size_t, std::string_view>> ParseChunkSizeLine(
        std::string_view line) {
        const std::size_t semicolon = line.find(';');
        const std::string_view sizeText = line.substr(0, semicolon);
        if (sizeText.empty() || sizeText.front() == ' ' || sizeText.back() == ' '
            || sizeText.front() == '\t' || sizeText.back() == '\t') {
            return LikesProgram::Status::InvalidArgument(u"invalid chunk size");
        }
        std::size_t size = 0;
        for (char ch : sizeText) {
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9') digit = static_cast<unsigned>(ch - '0');
            else if (ch >= 'A' && ch <= 'F') digit = static_cast<unsigned>(ch - 'A' + 10);
            else if (ch >= 'a' && ch <= 'f') digit = static_cast<unsigned>(ch - 'a' + 10);
            else return LikesProgram::Status::InvalidArgument(u"invalid chunk size");
            if (size > (std::numeric_limits<std::size_t>::max() - digit) / 16) {
                return LikesProgram::Status::InvalidArgument(u"chunk size overflow");
            }
            size = size * 16 + digit;
        }
        const std::string_view extensions = semicolon == std::string_view::npos
            ? std::string_view{} : line.substr(semicolon);
        if (!IsValidChunkExtensions(extensions)) {
            return LikesProgram::Status::InvalidArgument(u"invalid chunk extension");
        }
        return std::make_pair(size, extensions);
    }

    bool IsForbiddenTrailer(std::string_view name) noexcept {
        return EqualsIgnoreCase(name, "Content-Length")
            || EqualsIgnoreCase(name, "Transfer-Encoding")
            || EqualsIgnoreCase(name, "Host");
    }

    LikesProgram::Result<std::vector<HttpHeader>> ValidateTrailers(
        std::string_view block, const Http1MessageLimits& limits) {
        if (block.size() > limits.maxTrailerBytes) {
            return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                u"HTTP/1 trailers exceed limit");
        }
        auto trailers = ParseHeaderLines(block);
        if (!trailers.IsOk()) return trailers.GetStatus();
        for (const auto& trailer : trailers.Value()) {
            if (IsForbiddenTrailer(trailer.name)) {
                return LikesProgram::Status::InvalidArgument(u"forbidden HTTP trailer");
            }
        }
        return trailers;
    }

    struct ParsedEnvelope {
        std::string_view startLine;
        std::vector<HttpHeader> headers;
        std::vector<std::uint8_t> body;
        std::vector<HttpHeader> trailers;
    };

    LikesProgram::Result<ParsedEnvelope> ParseHttp1Envelope(
        std::string_view message,
        const Http1MessageLimits& limits,
        bool noBodyExpected = false) {
        const std::size_t headerEnd = message.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) {
            return LikesProgram::Status::InvalidArgument(u"HTTP/1 message missing header terminator");
        }
        if (headerEnd + 4 > limits.maxHeaderBytes) {
            return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                u"HTTP/1 headers exceed limit");
        }

        const std::string_view head = message.substr(0, headerEnd);
        const std::string_view bodyWire = message.substr(headerEnd + 4);
        const std::size_t firstLineEnd = head.find("\r\n");
        const std::string_view startLine = firstLineEnd == std::string_view::npos
            ? head : head.substr(0, firstLineEnd);
        const std::string_view headerBlock = firstLineEnd == std::string_view::npos
            ? std::string_view{} : head.substr(firstLineEnd + 2);
        auto headers = ParseHeaderLines(headerBlock);
        if (!headers.IsOk()) return headers.GetStatus();
        auto framing = ReadFraming(headers.Value());
        if (!framing.IsOk()) return framing.GetStatus();

        ParsedEnvelope result;
        result.startLine = startLine;
        result.headers = headers.MoveValue();
        if (noBodyExpected) {
            if (!bodyWire.empty()) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/1 response body is prohibited by request/status semantics");
            }
            return result;
        }
        if (framing.Value().chunked) {
            LikesProgram::Http::Http1ChunkedDecoder decoder(limits);
            auto fed = decoder.Feed(bodyWire);
            if (!fed.IsOk()) return fed.GetStatus();
            auto finished = decoder.Finish();
            if (!finished.IsOk()) return finished.GetStatus();
            result.body = decoder.Body();
            result.trailers = decoder.Trailers();
        } else {
            if (framing.Value().hasLength && framing.Value().contentLength != bodyWire.size()) {
                return LikesProgram::Status::InvalidArgument(u"HTTP/1 body length mismatch");
            }
            if (bodyWire.size() > limits.maxBodyBytes) {
                return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                    u"HTTP/1 body exceeds limit");
            }
            result.body.assign(
                reinterpret_cast<const std::uint8_t*>(bodyWire.data()),
                reinterpret_cast<const std::uint8_t*>(bodyWire.data() + bodyWire.size()));
        }
        return result;
    }

    bool IsNoBodyStatus(int statusCode) noexcept {
        return (statusCode >= 100 && statusCode < 200)
            || statusCode == 204 || statusCode == 304;
    }

    struct ParsedResponseLine {
        std::string version;
        int statusCode = 0;
        std::string reason;
    };

    LikesProgram::Result<ParsedResponseLine> ParseResponseLine(std::string_view startLine) {
        const std::size_t firstSpace = startLine.find(' ');
        if (firstSpace == std::string_view::npos) {
            return LikesProgram::Status::InvalidArgument(u"malformed HTTP status line");
        }
        const std::size_t secondSpace = startLine.find(' ', firstSpace + 1);
        const std::string_view codeText = secondSpace == std::string_view::npos
            ? startLine.substr(firstSpace + 1)
            : startLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        int code = 0;
        const char* begin = codeText.data();
        const char* end = begin + codeText.size();
        const auto parsed = std::from_chars(begin, end, code);
        if (parsed.ec != std::errc{} || parsed.ptr != end || code < 100 || code > 599) {
            return LikesProgram::Status::InvalidArgument(u"invalid HTTP status code");
        }
        ParsedResponseLine result;
        result.version = std::string(startLine.substr(0, firstSpace));
        result.statusCode = code;
        result.reason = secondSpace == std::string_view::npos
            ? std::string{} : std::string(startLine.substr(secondSpace + 1));
        if (!IsHttp1Version(result.version) || !IsSafeFieldValue(result.reason)) {
            return LikesProgram::Status::InvalidArgument(u"invalid HTTP status line");
        }
        return result;
    }

    bool HasHeader(const std::vector<HttpHeader>& headers, std::string_view name) {
        for (const auto& header : headers) {
            if (EqualsIgnoreCase(header.name, name)) return true;
        }
        return false;
    }

    LikesProgram::Result<void> AppendHeaders(
        std::string& output, const std::vector<HttpHeader>& headers) {
        for (const auto& header : headers) {
            if (!IsSafeToken(header.name)) {
                return LikesProgram::Status::InvalidArgument(u"invalid HTTP header name");
            }
            if (!IsSafeFieldValue(header.value)) {
                return LikesProgram::Status::InvalidArgument(u"invalid HTTP header value");
            }
            output.append(header.name).append(": ").append(header.value).append("\r\n");
        }
        return {};
    }

    LikesProgram::Result<void> AppendTrailers(
        std::string& output, const std::vector<HttpHeader>& trailers) {
        for (const auto& trailer : trailers) {
            if (IsForbiddenTrailer(trailer.name)) {
                return LikesProgram::Status::InvalidArgument(u"forbidden HTTP trailer");
            }
        }
        return AppendHeaders(output, trailers);
    }

    void AppendBody(std::string& output, const std::vector<std::uint8_t>& body) {
        if (!body.empty()) {
            output.append(reinterpret_cast<const char*>(body.data()), body.size());
        }
    }

    LikesProgram::Result<void> AppendChunkedBody(
        std::string& output,
        const std::vector<std::uint8_t>& body,
        const std::vector<HttpHeader>& trailers) {
        if (!body.empty()) {
            char sizeText[2 * sizeof(std::size_t) + 1]{};
            const auto converted = std::to_chars(
                sizeText, sizeText + sizeof(sizeText), body.size(), 16);
            if (converted.ec != std::errc{}) {
                return LikesProgram::Status::Internal(u"failed to encode chunk size");
            }
            output.append(sizeText, converted.ptr).append("\r\n");
            AppendBody(output, body);
            output.append("\r\n");
        }
        output.append("0\r\n");
        auto appended = AppendTrailers(output, trailers);
        if (!appended.IsOk()) return appended.GetStatus();
        output.append("\r\n");
        return {};
    }

    LikesProgram::Result<std::string> BuildEnvelope(
        std::string startLine,
        const std::vector<HttpHeader>& headers,
        const std::vector<std::uint8_t>& body,
        const std::vector<HttpHeader>& trailers,
        const Http1MessageLimits& limits,
        bool suppressBody = false,
        bool hypotheticalBodyLength = false) {
        auto framing = ReadFraming(headers);
        if (!framing.IsOk()) return framing.GetStatus();
        if (body.size() > limits.maxBodyBytes) {
            return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                u"HTTP/1 body exceeds limit");
        }
        if (!framing.Value().chunked && !trailers.empty()) {
            return LikesProgram::Status::InvalidArgument(
                u"HTTP trailers require chunked Transfer-Encoding");
        }
        if (!hypotheticalBodyLength
            && framing.Value().hasLength && framing.Value().contentLength != body.size()) {
            return LikesProgram::Status::InvalidArgument(u"HTTP/1 body length mismatch");
        }
        std::string output;
        output.reserve(startLine.size() + body.size() + 128);
        output.append(startLine).append("\r\n");
        auto appended = AppendHeaders(output, headers);
        if (!appended.IsOk()) return appended.GetStatus();
        if (!framing.Value().hasTransferEncoding && !body.empty()) {
            output.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
        }
        output.append("\r\n");
        if (!suppressBody) {
            if (framing.Value().chunked) {
                auto chunked = AppendChunkedBody(output, body, trailers);
                if (!chunked.IsOk()) return chunked.GetStatus();
            } else {
                AppendBody(output, body);
            }
        }
        return output;
    }

    bool HasConnectionToken(
        const std::vector<HttpHeader>& headers,
        std::string_view token) {
        for (const auto& header : headers) {
            if (!EqualsIgnoreCase(header.name, "Connection")) continue;
            std::size_t begin = 0;
            while (begin <= header.value.size()) {
                const std::size_t comma = header.value.find(',', begin);
                const std::size_t end = comma == std::string::npos
                    ? header.value.size() : comma;
                std::size_t first = begin;
                while (first < end && IsOptionalWhitespace(header.value[first])) ++first;
                std::size_t last = end;
                while (last > first && IsOptionalWhitespace(header.value[last - 1])) --last;
                if (EqualsIgnoreCase(
                        std::string_view(header.value).substr(first, last - first), token)) {
                    return true;
                }
                if (comma == std::string::npos) break;
                begin = comma + 1;
            }
        }
        return false;
    }

    bool HasNamedHeader(
        const std::vector<HttpHeader>& headers,
        std::string_view name) {
        for (const auto& header : headers) {
            if (EqualsIgnoreCase(header.name, name)) return true;
        }
        return false;
    }

    bool ShouldKeepAlive(
        std::string_view version,
        const std::vector<HttpHeader>& headers) {
        if (HasConnectionToken(headers, "close")) return false;
        if (version == "HTTP/1.0") return HasConnectionToken(headers, "keep-alive");
        return true;
    }

    LikesProgram::Result<std::optional<std::size_t>> FindChunkedMessageEnd(
        std::string_view message,
        std::size_t bodyStart,
        const Http1MessageLimits& limits) {
        std::size_t cursor = bodyStart;
        std::size_t bodyBytes = 0;
        std::size_t trailerBytes = 0;
        std::string trailerBlock;
        for (;;) {
            const std::size_t lineEnd = message.find("\r\n", cursor);
            if (lineEnd == std::string_view::npos) {
                if (message.size() - cursor > limits.maxChunkLineBytes) {
                    return LikesProgram::Status(StatusCode::ResourceExhausted,
                        u"chunk-size line exceeds limit");
                }
                return std::optional<std::size_t>{};
            }
            if (lineEnd - cursor > limits.maxChunkLineBytes) {
                return LikesProgram::Status(StatusCode::ResourceExhausted,
                    u"chunk-size line exceeds limit");
            }
            auto parsed = ParseChunkSizeLine(message.substr(cursor, lineEnd - cursor));
            if (!parsed.IsOk()) return parsed.GetStatus();
            const std::size_t chunkSize = parsed.Value().first;
            if (chunkSize > limits.maxBodyBytes || chunkSize > limits.maxBodyBytes - bodyBytes) {
                return LikesProgram::Status(StatusCode::ResourceExhausted,
                    u"HTTP/1 body exceeds limit");
            }
            bodyBytes += chunkSize;
            cursor = lineEnd + 2;
            if (chunkSize != 0) {
                if (message.size() - cursor < chunkSize + 2) {
                    return std::optional<std::size_t>{};
                }
                if (message.compare(cursor + chunkSize, 2, "\r\n") != 0) {
                    return LikesProgram::Status::InvalidArgument(
                        u"chunk data missing CRLF");
                }
                cursor += chunkSize + 2;
                continue;
            }
            for (;;) {
                const std::size_t trailerEnd = message.find("\r\n", cursor);
                if (trailerEnd == std::string_view::npos) {
                    if (trailerBytes + message.size() - cursor > limits.maxTrailerBytes) {
                        return LikesProgram::Status(StatusCode::ResourceExhausted,
                            u"HTTP/1 trailers exceed limit");
                    }
                    return std::optional<std::size_t>{};
                }
                if (trailerEnd == cursor) {
                    auto trailers = ValidateTrailers(trailerBlock, limits);
                    if (!trailers.IsOk()) return trailers.GetStatus();
                    return std::optional<std::size_t>(trailerEnd + 2);
                }
                if (trailerBytes + trailerEnd - cursor + 2 > limits.maxTrailerBytes) {
                    return LikesProgram::Status(StatusCode::ResourceExhausted,
                        u"HTTP/1 trailers exceed limit");
                }
                trailerBlock.append(message.substr(cursor, trailerEnd - cursor + 2));
                trailerBytes += trailerEnd - cursor + 2;
                cursor = trailerEnd + 2;
            }
        }
    }

    LikesProgram::Result<std::optional<std::size_t>> FindHttp1MessageEnd(
        std::string_view message,
        Http1MessageKind kind,
        const Http1MessageLimits& limits,
        const LikesProgram::Http::Http1ResponseContext& context,
        bool endOfInput) {
        const std::size_t headerEnd = message.find("\r\n\r\n");
        if (headerEnd == std::string_view::npos) {
            if (message.size() > limits.maxHeaderBytes) {
                return LikesProgram::Status(StatusCode::ResourceExhausted,
                    u"HTTP/1 headers exceed limit");
            }
            return std::optional<std::size_t>{};
        }
        if (headerEnd + 4 > limits.maxHeaderBytes) {
            return LikesProgram::Status(StatusCode::ResourceExhausted,
                u"HTTP/1 headers exceed limit");
        }
        const std::string_view head = message.substr(0, headerEnd);
        const std::size_t firstLineEnd = head.find("\r\n");
        const std::string_view startLine = firstLineEnd == std::string_view::npos
            ? head : head.substr(0, firstLineEnd);
        const std::string_view headerBlock = firstLineEnd == std::string_view::npos
            ? std::string_view{} : head.substr(firstLineEnd + 2);
        auto headers = ParseHeaderLines(headerBlock);
        if (!headers.IsOk()) return headers.GetStatus();
        auto framing = ReadFraming(headers.Value());
        if (!framing.IsOk()) return framing.GetStatus();

        const std::size_t bodyStart = headerEnd + 4;
        bool noBody = false;
        if (kind == Http1MessageKind::Response) {
            auto line = ParseResponseLine(startLine);
            if (!line.IsOk()) return line.GetStatus();
            noBody = context.requestWasHead
                || (context.requestWasConnect
                    && line.Value().statusCode >= 200
                    && line.Value().statusCode < 300)
                || IsNoBodyStatus(line.Value().statusCode);
            const bool framingForbidden = (line.Value().statusCode >= 100
                && line.Value().statusCode < 200) || line.Value().statusCode == 204;
            if (framingForbidden
                && (HasNamedHeader(headers.Value(), "Content-Length")
                    || HasNamedHeader(headers.Value(), "Transfer-Encoding"))) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/1 no-body status must not carry framing headers");
            }
        }
        if (noBody) return std::optional<std::size_t>(bodyStart);
        if (framing.Value().chunked) {
            auto end = FindChunkedMessageEnd(message, bodyStart, limits);
            if (!end.IsOk() || end.Value().has_value()) return end;
            return std::optional<std::size_t>{};
        }
        if (framing.Value().hasLength) {
            const std::size_t length = framing.Value().contentLength;
            if (length > limits.maxBodyBytes) {
                return LikesProgram::Status(StatusCode::ResourceExhausted,
                    u"HTTP/1 body exceeds limit");
            }
            if (message.size() - bodyStart < length) {
                return std::optional<std::size_t>{};
            }
            return std::optional<std::size_t>(bodyStart + length);
        }
        if (kind == Http1MessageKind::Request) return std::optional<std::size_t>(bodyStart);
        if (!endOfInput) return std::optional<std::size_t>{};
        if (message.size() - bodyStart > limits.maxBodyBytes) {
            return LikesProgram::Status(StatusCode::ResourceExhausted,
                u"HTTP/1 body exceeds limit");
        }
        return std::optional<std::size_t>(message.size());
    }
}

namespace LikesProgram {
    namespace Http {
        struct Http1ChunkedDecoder::Impl {
            enum class State {
                ChunkSize,
                ChunkData,
                ChunkDataCrlf,
                Trailers,
                Complete,
                Cancelled,
                Failed
            };

            explicit Impl(Http1MessageLimits value) : limits(value) { }

            Result<void> Fail(const Status& status) {
                state = State::Failed;
                error = status;
                return error;
            }

            Http1MessageLimits limits;
            State state = State::ChunkSize;
            std::string pending;
            std::string trailerBlock;
            std::size_t remaining = 0;
            std::size_t receivedBodyBytes = 0;
            std::vector<std::uint8_t> body;
            std::vector<HttpHeader> trailers;
            HttpBodySink* sink = nullptr;
            Status error;
        };

        Http1ChunkedDecoder::Http1ChunkedDecoder(Http1MessageLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) { }

        Http1ChunkedDecoder::~Http1ChunkedDecoder() = default;
        Http1ChunkedDecoder::Http1ChunkedDecoder(Http1ChunkedDecoder&&) noexcept = default;
        Http1ChunkedDecoder& Http1ChunkedDecoder::operator=(Http1ChunkedDecoder&&) noexcept = default;

        Result<void> Http1ChunkedDecoder::Feed(std::string_view bytes) {
            if (m_impl->state == Impl::State::Failed) return m_impl->error;
            if (m_impl->state == Impl::State::Cancelled) return m_impl->error;
            if (m_impl->state == Impl::State::Complete) {
                return m_impl->Fail(Status::InvalidArgument(u"bytes after chunked message"));
            }
            m_impl->pending.append(bytes.data(), bytes.size());
            for (;;) {
                if (m_impl->state == Impl::State::ChunkSize) {
                    const std::size_t end = m_impl->pending.find("\r\n");
                    if (end == std::string::npos) {
                        if (m_impl->pending.size() > m_impl->limits.maxChunkLineBytes) {
                            return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                                u"chunk-size line exceeds limit"));
                        }
                        return {};
                    }
                    if (end > m_impl->limits.maxChunkLineBytes) {
                        return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                            u"chunk-size line exceeds limit"));
                    }
                    auto parsed = ParseChunkSizeLine(std::string_view(m_impl->pending).substr(0, end));
                    if (!parsed.IsOk()) return m_impl->Fail(parsed.GetStatus());
                    m_impl->pending.erase(0, end + 2);
                    if (parsed.Value().first > m_impl->limits.maxBodyBytes
                        || m_impl->receivedBodyBytes > m_impl->limits.maxBodyBytes
                        || parsed.Value().first > m_impl->limits.maxBodyBytes
                            - m_impl->receivedBodyBytes) {
                        return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                            u"HTTP/1 body exceeds limit"));
                    }
                    m_impl->remaining = parsed.Value().first;
                    m_impl->state = m_impl->remaining == 0
                        ? Impl::State::Trailers : Impl::State::ChunkData;
                    continue;
                }
                if (m_impl->state == Impl::State::ChunkData) {
                    const std::size_t take = std::min(m_impl->remaining, m_impl->pending.size());
                    if (m_impl->sink != nullptr) {
                        if (take > m_impl->sink->WritableBytes()) {
                            return Status(StatusCode::ResourceExhausted,
                                u"HTTP/1 body sink cannot accept the pending chunk data");
                        }
                        if (take != 0) {
                            auto pushed = m_impl->sink->Push(
                                reinterpret_cast<const std::uint8_t*>(m_impl->pending.data()),
                                take);
                            if (!pushed.IsOk()) return m_impl->Fail(pushed.GetStatus());
                            if (pushed.Value() != take) {
                                m_impl->sink->Cancel();
                                return m_impl->Fail(Status::Internal(
                                    u"HTTP/1 body sink accepted a partial chunk unexpectedly"));
                            }
                        }
                    } else {
                        m_impl->body.insert(
                            m_impl->body.end(),
                            reinterpret_cast<const std::uint8_t*>(m_impl->pending.data()),
                            reinterpret_cast<const std::uint8_t*>(m_impl->pending.data() + take));
                    }
                    m_impl->pending.erase(0, take);
                    m_impl->remaining -= take;
                    m_impl->receivedBodyBytes += take;
                    if (m_impl->remaining != 0) return {};
                    m_impl->state = Impl::State::ChunkDataCrlf;
                    continue;
                }
                if (m_impl->state == Impl::State::ChunkDataCrlf) {
                    if (m_impl->pending.size() < 2) return {};
                    if (m_impl->pending.compare(0, 2, "\r\n") != 0) {
                        return m_impl->Fail(Status::InvalidArgument(u"chunk data missing CRLF"));
                    }
                    m_impl->pending.erase(0, 2);
                    m_impl->state = Impl::State::ChunkSize;
                    continue;
                }
                if (m_impl->state == Impl::State::Trailers) {
                    const std::size_t end = m_impl->pending.find("\r\n");
                    if (end == std::string::npos) {
                        if (m_impl->trailerBlock.size() + m_impl->pending.size()
                            > m_impl->limits.maxTrailerBytes) {
                            return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                                u"HTTP/1 trailers exceed limit"));
                        }
                        return {};
                    }
                    if (end == 0) {
                        m_impl->pending.erase(0, 2);
                        auto trailers = ValidateTrailers(m_impl->trailerBlock, m_impl->limits);
                        if (!trailers.IsOk()) return m_impl->Fail(trailers.GetStatus());
                        m_impl->trailers = trailers.MoveValue();
                        if (!m_impl->pending.empty()) {
                            return m_impl->Fail(Status::InvalidArgument(
                                u"bytes after chunked message"));
                        }
                        if (m_impl->sink != nullptr) {
                            auto closed = m_impl->sink->Close();
                            if (!closed.IsOk()) return m_impl->Fail(closed.GetStatus());
                        }
                        m_impl->state = Impl::State::Complete;
                        return {};
                    }
                    if (m_impl->trailerBlock.size() + end + 2 > m_impl->limits.maxTrailerBytes) {
                        return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                            u"HTTP/1 trailers exceed limit"));
                    }
                    m_impl->trailerBlock.append(m_impl->pending, 0, end + 2);
                    m_impl->pending.erase(0, end + 2);
                    continue;
                }
                return {};
            }
        }

        Result<void> Http1ChunkedDecoder::Finish() {
            if (m_impl->state == Impl::State::Failed) return m_impl->error;
            if (m_impl->state == Impl::State::Cancelled) return m_impl->error;
            if (m_impl->state != Impl::State::Complete) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"incomplete chunked message"));
            }
            return {};
        }

        Result<void> Http1ChunkedDecoder::AttachBodySink(HttpBodySink* sink) {
            if (sink == nullptr) {
                return Status::InvalidArgument(u"HTTP/1 body sink is null");
            }
            if (m_impl->state == Impl::State::Failed
                || m_impl->state == Impl::State::Cancelled) {
                return m_impl->error;
            }
            if (m_impl->state != Impl::State::ChunkSize
                || m_impl->receivedBodyBytes != 0
                || !m_impl->body.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/1 body sink must be attached before chunk data");
            }
            m_impl->sink = sink;
            return {};
        }

        bool Http1ChunkedDecoder::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->sink != nullptr;
        }

        void Http1ChunkedDecoder::Cancel() noexcept {
            Cancel(HttpBodyCancelReason::Application);
        }

        void Http1ChunkedDecoder::Cancel(HttpBodyCancelReason reason) noexcept {
            if (m_impl == nullptr
                || m_impl->state == Impl::State::Complete
                || m_impl->state == Impl::State::Failed
                || m_impl->state == Impl::State::Cancelled) {
                return;
            }
            if (m_impl->sink != nullptr) m_impl->sink->Cancel(reason);
            m_impl->state = Impl::State::Cancelled;
            m_impl->error = Status(StatusCode::Cancelled,
                u"HTTP/1 chunked body was cancelled");
        }

        bool Http1ChunkedDecoder::IsComplete() const noexcept {
            return m_impl->state == Impl::State::Complete;
        }

        bool Http1ChunkedDecoder::IsCancelled() const noexcept {
            return m_impl != nullptr && m_impl->state == Impl::State::Cancelled;
        }

        const std::vector<std::uint8_t>& Http1ChunkedDecoder::Body() const noexcept {
            return m_impl->body;
        }

        const std::vector<HttpHeader>& Http1ChunkedDecoder::Trailers() const noexcept {
            return m_impl->trailers;
        }

        void Http1ChunkedDecoder::Reset() noexcept {
            if (m_impl->sink != nullptr) m_impl->sink->Reset();
            m_impl->state = Impl::State::ChunkSize;
            m_impl->pending.clear();
            m_impl->trailerBlock.clear();
            m_impl->remaining = 0;
            m_impl->receivedBodyBytes = 0;
            m_impl->body.clear();
            m_impl->trailers.clear();
            m_impl->error = Status{};
        }

        Result<HttpRequest> ParseHttp1Request(std::string_view message) {
            return ParseHttp1Request(message, Http1MessageLimits{});
        }

        Result<HttpRequest> ParseHttp1Request(
            std::string_view message, const Http1MessageLimits& limits) {
            auto envelope = ParseHttp1Envelope(message, limits);
            if (!envelope.IsOk()) return envelope.GetStatus();
            const std::string_view startLine = envelope.Value().startLine;
            const std::size_t firstSpace = startLine.find(' ');
            const std::size_t secondSpace = firstSpace == std::string_view::npos
                ? std::string_view::npos : startLine.find(' ', firstSpace + 1);
            if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) {
                return Status::InvalidArgument(u"malformed HTTP request line");
            }
            HttpRequest request;
            request.method = std::string(startLine.substr(0, firstSpace));
            request.target = std::string(startLine.substr(
                firstSpace + 1, secondSpace - firstSpace - 1));
            request.version = std::string(startLine.substr(secondSpace + 1));
            request.headers = envelope.Value().headers;
            request.body = envelope.Value().body;
            request.trailers = envelope.Value().trailers;
            if (!IsSafeToken(request.method) || !IsSafeRequestTarget(request.target)
                || !IsHttp1Version(request.version)) {
                return Status::InvalidArgument(u"invalid HTTP request line");
            }
            auto framing = ReadFraming(request.headers);
            if (!framing.IsOk()) return framing.GetStatus();
            if (!framing.Value().hasLength && !framing.Value().chunked && !request.body.empty()) {
                return Status::InvalidArgument(u"HTTP/1 request body requires framing");
            }
            return request;
        }

        Result<HttpResponse> ParseHttp1Response(std::string_view message) {
            return ParseHttp1Response(message, Http1MessageLimits{}, Http1ResponseContext{});
        }

        Result<HttpResponse> ParseHttp1Response(
            std::string_view message, const Http1MessageLimits& limits) {
            return ParseHttp1Response(message, limits, Http1ResponseContext{});
        }

        Result<HttpResponse> ParseHttp1Response(
            std::string_view message,
            const Http1MessageLimits& limits,
            const Http1ResponseContext& context) {
            if (context.requestWasHead && context.requestWasConnect) {
                return Status::InvalidArgument(u"HTTP/1 response context is ambiguous");
            }
            const std::size_t firstLineEnd = message.find("\r\n");
            if (firstLineEnd == std::string_view::npos) {
                return Status::InvalidArgument(u"HTTP/1 message missing headers");
            }
            auto line = ParseResponseLine(message.substr(0, firstLineEnd));
            if (!line.IsOk()) return line.GetStatus();
            const bool connectTunnel = context.requestWasConnect
                && line.Value().statusCode >= 200 && line.Value().statusCode < 300;
            const bool noBody = context.requestWasHead
                || connectTunnel || IsNoBodyStatus(line.Value().statusCode);
            auto envelope = ParseHttp1Envelope(message, limits, noBody);
            if (!envelope.IsOk()) return envelope.GetStatus();
            const bool framingForbidden = (line.Value().statusCode >= 100
                && line.Value().statusCode < 200) || line.Value().statusCode == 204;
            if (framingForbidden
                && (HasHeader(envelope.Value().headers, "Content-Length")
                    || HasHeader(envelope.Value().headers, "Transfer-Encoding"))) {
                return Status::InvalidArgument(
                    u"HTTP/1 no-body status must not carry framing headers");
            }
            HttpResponse response;
            response.version = line.Value().version;
            response.statusCode = line.Value().statusCode;
            response.reason = line.Value().reason;
            response.headers = envelope.Value().headers;
            response.body = envelope.Value().body;
            response.trailers = envelope.Value().trailers;
            return response;
        }

        Result<std::string> BuildHttp1Request(const HttpRequest& request) {
            return BuildHttp1Request(request, Http1MessageLimits{});
        }

        Result<std::string> BuildHttp1Request(
            const HttpRequest& request, const Http1MessageLimits& limits) {
            if (!IsSafeToken(request.method) || !IsSafeRequestTarget(request.target)
                || !IsHttp1Version(request.version)) {
                return Status::InvalidArgument(u"invalid HTTP request fields");
            }
            return BuildEnvelope(
                request.method + " " + request.target + " " + request.version,
                request.headers, request.body, request.trailers, limits);
        }

        Result<std::string> BuildHttp1Response(const HttpResponse& response) {
            return BuildHttp1Response(response, Http1MessageLimits{}, Http1ResponseContext{});
        }

        Result<std::string> BuildHttp1Response(
            const HttpResponse& response, const Http1MessageLimits& limits) {
            return BuildHttp1Response(response, limits, Http1ResponseContext{});
        }

        Result<std::string> BuildHttp1Response(
            const HttpResponse& response,
            const Http1MessageLimits& limits,
            const Http1ResponseContext& context) {
            if (!IsHttp1Version(response.version)
                || response.statusCode < 100 || response.statusCode > 599
                || !IsSafeFieldValue(response.reason)) {
                return Status::InvalidArgument(u"invalid HTTP response fields");
            }
            if (context.requestWasHead && context.requestWasConnect) {
                return Status::InvalidArgument(u"HTTP/1 response context is ambiguous");
            }
            const bool connectTunnel = context.requestWasConnect
                && response.statusCode >= 200 && response.statusCode < 300;
            const bool noBody = context.requestWasHead
                || connectTunnel || IsNoBodyStatus(response.statusCode);
            if (noBody && !response.body.empty()) {
                if (!context.requestWasHead) {
                    return Status::InvalidArgument(
                        u"HTTP/1 response body is prohibited by status semantics");
                }
            }
            const bool framingForbidden = (response.statusCode >= 100
                && response.statusCode < 200) || response.statusCode == 204;
            if ((connectTunnel || framingForbidden)
                && (!response.body.empty() || !response.trailers.empty()
                    || (framingForbidden
                        && HasHeader(response.headers, "Content-Length"))
                    || (framingForbidden
                        && HasHeader(response.headers, "Transfer-Encoding")))) {
                return Status::InvalidArgument(
                    u"HTTP/1 no-body response has body or framing fields");
            }
            if ((context.requestWasHead || response.statusCode == 304)
                && !response.trailers.empty()) {
                return Status::InvalidArgument(
                    u"HTTP/1 no-body response cannot emit trailers");
            }
            return BuildEnvelope(
                response.version + " " + std::to_string(response.statusCode) + " " + response.reason,
                response.headers,
                response.body,
                response.trailers,
                limits,
                noBody,
                response.statusCode == 304 && !context.requestWasHead);
        }

        Result<Http1RequestTargetForm> ClassifyHttp1RequestTarget(
            std::string_view method,
            std::string_view target) {
            if (target.empty()) {
                return Status::InvalidArgument(u"HTTP/1 request target is empty");
            }
            for (const char byte : target) {
                if (IsOptionalWhitespace(byte) || static_cast<unsigned char>(byte) < 0x20) {
                    return Status::InvalidArgument(u"HTTP/1 request target contains whitespace");
                }
            }
            if (EqualsIgnoreCase(method, "CONNECT")) {
                if (target == "*" || target.front() == '/' || target.find("://") != std::string_view::npos) {
                    return Status::InvalidArgument(u"CONNECT requires authority-form target");
                }
                return Http1RequestTargetForm::Authority;
            }
            if (target == "*") return Http1RequestTargetForm::Asterisk;
            if (target.find("://") != std::string_view::npos) {
                return Http1RequestTargetForm::Absolute;
            }
            if (target.front() == '/') return Http1RequestTargetForm::Origin;
            return Status::InvalidArgument(u"HTTP/1 request target is not a supported form");
        }

        Result<Http1ConnectionDecision> EvaluateHttp1Connection(
            const HttpRequest& request,
            const HttpResponse& response) {
            auto target = ClassifyHttp1RequestTarget(request.method, request.target);
            if (!target.IsOk()) return target.GetStatus();
            Http1ConnectionDecision decision;
            decision.keepAlive = ShouldKeepAlive(request.version, request.headers)
                && ShouldKeepAlive(response.version, response.headers);
            decision.tunnel = EqualsIgnoreCase(request.method, "CONNECT")
                && response.statusCode >= 200 && response.statusCode < 300;
            const bool requestUpgrade = HasConnectionToken(request.headers, "upgrade")
                && HasNamedHeader(request.headers, "Upgrade");
            const bool responseUpgrade = response.statusCode == 101
                && HasConnectionToken(response.headers, "upgrade")
                && HasNamedHeader(response.headers, "Upgrade");
            if (response.statusCode == 101 && (!requestUpgrade || !responseUpgrade)) {
                return Status::InvalidArgument(
                    u"HTTP/1 101 response requires a matching Upgrade request");
            }
            decision.upgrade = responseUpgrade;
            if (decision.tunnel || decision.upgrade) decision.keepAlive = false;
            if (!decision.keepAlive) decision.closeDelimited = false;
            else if (!decision.tunnel && !decision.upgrade
                && !EqualsIgnoreCase(request.method, "HEAD")
                && !IsNoBodyStatus(response.statusCode)
                && !HasNamedHeader(response.headers, "Content-Length")
                && !HasNamedHeader(response.headers, "Transfer-Encoding")) {
                decision.closeDelimited = true;
            }
            return decision;
        }

        struct Http1Connection::Impl {
            explicit Impl(Http1MessageKind value, Http1MessageLimits messageLimits)
                : kind(value), limits(messageLimits) { }

            Status RecordError(
                Status status,
                HttpErrorOrigin origin,
                std::size_t byteOffset = 0) {
                lastError = status;
                errorContext = {
                    true,
                    HttpVersion::Http1,
                    HttpErrorScope::Connection,
                    origin,
                    0,
                    HttpErrorUnitKind::None,
                    0,
                    byteOffset,
                    status.Code()
                };
                return lastError;
            }

            Http1MessageKind kind;
            Http1MessageLimits limits;
            Http1ConnectionState state = Http1ConnectionState::Open;
            std::string pending;
            bool finished = false;
            Status lastError;
            HttpErrorContext errorContext;
        };

        Http1Connection::Http1Connection(Http1MessageKind kind, Http1MessageLimits limits)
            : m_impl(std::make_unique<Impl>(kind, limits)) { }

        Http1Connection::~Http1Connection() = default;
        Http1Connection::Http1Connection(Http1Connection&&) noexcept = default;
        Http1Connection& Http1Connection::operator=(Http1Connection&&) noexcept = default;

        Result<void> Http1Connection::Feed(std::string_view bytes) {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/1 connection is uninitialized");
            if (m_impl->finished || m_impl->state == Http1ConnectionState::Closed) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition, u"HTTP/1 connection is closed"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            if (m_impl->state == Http1ConnectionState::Upgraded
                || m_impl->state == Http1ConnectionState::Tunnel) {
                m_impl->pending.append(bytes.data(), bytes.size());
                return {};
            }
            if (bytes.size() > m_impl->limits.maxHeaderBytes + m_impl->limits.maxBodyBytes
                || m_impl->pending.size() > m_impl->limits.maxHeaderBytes
                    + m_impl->limits.maxBodyBytes - bytes.size()) {
                return m_impl->RecordError(
                    Status(StatusCode::ResourceExhausted,
                        u"HTTP/1 connection buffered bytes exceed limit"),
                    HttpErrorOrigin::Resource,
                    m_impl->pending.size());
            }
            m_impl->pending.append(bytes.data(), bytes.size());
            return {};
        }

        Result<std::optional<HttpRequest>> Http1Connection::NextRequest() {
            if (!m_impl) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/1 connection is not a request reader");
            }
            if (m_impl->kind != Http1MessageKind::Request) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/1 connection is not a request reader"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            if (m_impl->state == Http1ConnectionState::Closed) {
                return std::optional<HttpRequest>{};
            }
            if (m_impl->state == Http1ConnectionState::Upgraded
                || m_impl->state == Http1ConnectionState::Tunnel) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/1 connection no longer accepts requests"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            if (m_impl->state == Http1ConnectionState::Closing) {
                if (m_impl->pending.empty()) return std::optional<HttpRequest>{};
                return m_impl->RecordError(
                    Status::InvalidArgument(
                        u"HTTP/1 bytes remain after a connection-close request"),
                    HttpErrorOrigin::Protocol,
                    m_impl->pending.size());
            }
            auto end = FindHttp1MessageEnd(
                m_impl->pending, Http1MessageKind::Request, m_impl->limits,
                Http1ResponseContext{}, m_impl->finished);
            if (!end.IsOk()) {
                return m_impl->RecordError(
                    end.GetStatus(), HttpErrorOrigin::Protocol, m_impl->pending.size());
            }
            if (!end.Value().has_value()) {
                if (m_impl->finished) {
                    return m_impl->RecordError(
                        Status::InvalidArgument(u"incomplete HTTP/1 request"),
                        HttpErrorOrigin::Protocol,
                        m_impl->pending.size());
                }
                return std::optional<HttpRequest>{};
            }
            const std::size_t messageEnd = *end.Value();
            auto parsed = ParseHttp1Request(
                std::string_view(m_impl->pending).substr(0, messageEnd), m_impl->limits);
            if (!parsed.IsOk()) {
                return m_impl->RecordError(
                    parsed.GetStatus(), HttpErrorOrigin::Protocol, messageEnd);
            }
            HttpRequest request = parsed.MoveValue();
            auto target = ClassifyHttp1RequestTarget(request.method, request.target);
            if (!target.IsOk()) {
                return m_impl->RecordError(
                    target.GetStatus(), HttpErrorOrigin::Protocol, messageEnd);
            }
            m_impl->pending.erase(0, messageEnd);
            if (!ShouldKeepAlive(request.version, request.headers)) {
                m_impl->state = Http1ConnectionState::Closing;
            }
            return std::optional<HttpRequest>(std::move(request));
        }

        Result<std::optional<HttpResponse>> Http1Connection::NextResponse(
            const Http1ResponseContext& context) {
            if (!m_impl) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/1 connection is not a response reader");
            }
            if (m_impl->kind != Http1MessageKind::Response) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/1 connection is not a response reader"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            if (m_impl->state == Http1ConnectionState::Closed) {
                return std::optional<HttpResponse>{};
            }
            if (m_impl->state == Http1ConnectionState::Upgraded
                || m_impl->state == Http1ConnectionState::Tunnel) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/1 connection no longer accepts responses"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            if (m_impl->state == Http1ConnectionState::Closing) {
                if (m_impl->pending.empty()) return std::optional<HttpResponse>{};
                return m_impl->RecordError(
                    Status::InvalidArgument(
                        u"HTTP/1 bytes remain after a connection-close response"),
                    HttpErrorOrigin::Protocol,
                    m_impl->pending.size());
            }
            auto end = FindHttp1MessageEnd(
                m_impl->pending, Http1MessageKind::Response, m_impl->limits,
                context, m_impl->finished);
            if (!end.IsOk()) {
                return m_impl->RecordError(
                    end.GetStatus(), HttpErrorOrigin::Protocol, m_impl->pending.size());
            }
            if (!end.Value().has_value()) {
                if (m_impl->finished) {
                    return m_impl->RecordError(
                        Status::InvalidArgument(u"incomplete HTTP/1 response"),
                        HttpErrorOrigin::Protocol,
                        m_impl->pending.size());
                }
                return std::optional<HttpResponse>{};
            }
            const std::size_t messageEnd = *end.Value();
            auto parsed = ParseHttp1Response(
                std::string_view(m_impl->pending).substr(0, messageEnd), m_impl->limits, context);
            if (!parsed.IsOk()) {
                return m_impl->RecordError(
                    parsed.GetStatus(), HttpErrorOrigin::Protocol, messageEnd);
            }
            HttpResponse response = parsed.MoveValue();
            if (response.statusCode == 101
                && (!context.requestWantsUpgrade
                    || !HasConnectionToken(response.headers, "upgrade")
                    || !HasNamedHeader(response.headers, "Upgrade"))) {
                return m_impl->RecordError(
                    Status::InvalidArgument(
                        u"HTTP/1 101 response requires a matching Upgrade context"),
                    HttpErrorOrigin::Protocol,
                    messageEnd);
            }
            m_impl->pending.erase(0, messageEnd);
            const bool tunnel = context.requestWasConnect
                && response.statusCode >= 200 && response.statusCode < 300;
            const bool upgrade = context.requestWantsUpgrade && response.statusCode == 101;
            if (tunnel) {
                m_impl->state = Http1ConnectionState::Tunnel;
            } else if (upgrade) {
                m_impl->state = Http1ConnectionState::Upgraded;
            } else if (!ShouldKeepAlive(response.version, response.headers)
                || (!IsNoBodyStatus(response.statusCode)
                    && !HasNamedHeader(response.headers, "Content-Length")
                    && !HasNamedHeader(response.headers, "Transfer-Encoding"))) {
                m_impl->state = Http1ConnectionState::Closing;
            }
            if (m_impl->finished && m_impl->pending.empty()
                && m_impl->state == Http1ConnectionState::Closing) {
                m_impl->state = Http1ConnectionState::Closed;
            }
            return std::optional<HttpResponse>(std::move(response));
        }

        Result<void> Http1Connection::Finish() {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/1 connection is uninitialized");
            if (m_impl->finished) return {};
            m_impl->finished = true;
            if (m_impl->pending.empty()
                && (m_impl->state == Http1ConnectionState::Open
                    || m_impl->state == Http1ConnectionState::Closing)) {
                m_impl->state = Http1ConnectionState::Closed;
            }
            return {};
        }

        Result<void> Http1Connection::Drain() {
            if (!m_impl) return Status(StatusCode::FailedPrecondition,
                u"HTTP/1 connection is uninitialized");
            if (m_impl->state == Http1ConnectionState::Upgraded
                || m_impl->state == Http1ConnectionState::Tunnel) {
                return m_impl->RecordError(
                    Status(StatusCode::FailedPrecondition,
                        u"HTTP/1 upgraded connection must be drained by its adapter"),
                    HttpErrorOrigin::Lifecycle,
                    m_impl->pending.size());
            }
            m_impl->pending.clear();
            m_impl->finished = true;
            m_impl->state = Http1ConnectionState::Closed;
            return {};
        }

        Http1ConnectionState Http1Connection::State() const noexcept {
            return m_impl ? m_impl->state : Http1ConnectionState::Closed;
        }

        std::size_t Http1Connection::BufferedBytes() const noexcept {
            return m_impl ? m_impl->pending.size() : 0;
        }

        Http1ConnectionSnapshot Http1Connection::Snapshot() const noexcept {
            if (!m_impl) return {};
            return {
                m_impl->kind,
                m_impl->state,
                m_impl->limits,
                m_impl->pending.size(),
                m_impl->finished
            };
        }

        HttpErrorContext Http1Connection::LastHttpErrorContext() const noexcept {
            return m_impl ? m_impl->errorContext : HttpErrorContext{};
        }

        Status Http1Connection::LastError() const {
            return m_impl ? m_impl->lastError
                : Status::Internal(u"HTTP/1 connection is uninitialized");
        }

        std::string Http1Connection::TakeBufferedBytes() {
            if (!m_impl) return {};
            std::string result;
            result.swap(m_impl->pending);
            return result;
        }

        void Http1Connection::Reset() noexcept {
            if (!m_impl) return;
            m_impl->state = Http1ConnectionState::Open;
            m_impl->pending.clear();
            m_impl->finished = false;
            m_impl->lastError = Status();
            m_impl->errorContext = {};
        }

        std::string HttpHeaderValue(
            const std::vector<HttpHeader>& headers, std::string_view name) {
            for (const auto& header : headers) {
                if (EqualsIgnoreCase(header.name, name)) return header.value;
            }
            return {};
        }
    }
}
