#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http1.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstdint>
#include <vector>

namespace LikesProgram {
    namespace Http {
        enum class HttpHeaderBlockProtocol : std::uint8_t {
            Http2,
            Http3
        };

        struct HttpHeaderBlockOptions {
            bool isRequest = true;
            bool isTrailer = false;
        };

        // Validates decoded HTTP/2 or HTTP/3 fields; HPACK/QPACK bytes stay external.
        LIKESPROGRAM_HTTP_API Result<void> ValidateHttpHeaderBlock(
            const std::vector<HttpHeader>& headers,
            HttpHeaderBlockProtocol protocol,
            HttpHeaderBlockOptions options = {});

        inline Result<void> ValidateHttp2HeaderBlock(
            const std::vector<HttpHeader>& headers,
            HttpHeaderBlockOptions options = {}) {
            return ValidateHttpHeaderBlock(headers, HttpHeaderBlockProtocol::Http2, options);
        }

        inline Result<void> ValidateHttp3HeaderBlock(
            const std::vector<HttpHeader>& headers,
            HttpHeaderBlockOptions options = {}) {
            return ValidateHttpHeaderBlock(headers, HttpHeaderBlockProtocol::Http3, options);
        }
    }
}
