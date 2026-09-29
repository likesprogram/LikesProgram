#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpHeaderBlock.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Http {
        struct Http2HpackLimits {
            std::size_t maxDynamicTableBytes = 4096;
            std::size_t maxHeaderListBytes   = 64 * 1024;
            std::size_t maxHeaderCount       = 256;
            std::size_t maxStringBytes       = 16 * 1024;
        };

        struct Http2HpackSnapshot {
            std::size_t decoderDynamicTableBytes = 0;
            std::size_t decoderDynamicTableEntries = 0;
            std::size_t decoderDynamicTableLimit = 0;
            std::size_t encoderDynamicTableBytes = 0;
            std::size_t encoderDynamicTableEntries = 0;
            std::size_t encoderDynamicTableLimit = 0;
        };

        enum class Http2HpackIndexingPolicy : std::uint8_t {
            Incremental,
            WithoutIndexing,
            NeverIndexed
        };

        struct Http2HpackField {
            HttpHeader header;
            Http2HpackIndexingPolicy indexing = Http2HpackIndexingPolicy::WithoutIndexing;
        };

        struct Http2HpackFailureActions {
            std::uint32_t connectionErrorCode = 0x9; // RFC 7540 COMPRESSION_ERROR
            bool closeConnection = true;
        };

        LIKESPROGRAM_HTTP_API Result<Http2HpackFailureActions> MapHttp2HpackFailure(const Status& failure);

        // Stateful RFC 7541 codec. Encoder and decoder dynamic tables are
        // independent, matching the two directions of one HTTP/2 connection.
        class Http2HpackCodec {
        public:
            LIKESPROGRAM_HTTP_API explicit Http2HpackCodec(Http2HpackLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http2HpackCodec();

            LIKESPROGRAM_HTTP_API Http2HpackCodec(Http2HpackCodec&&) noexcept;
            LIKESPROGRAM_HTTP_API Http2HpackCodec& operator=(Http2HpackCodec&&) noexcept;
            Http2HpackCodec(const Http2HpackCodec&) = delete;
            Http2HpackCodec& operator=(const Http2HpackCodec&) = delete;

            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> Decode(const std::uint8_t* data, std::size_t size);
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> Decode(const std::vector<std::uint8_t>& data);
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeHeaderBlock(const std::uint8_t* data, std::size_t size, HttpHeaderBlockOptions options = {});
            LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeHeaderBlock(const std::vector<std::uint8_t>& data, HttpHeaderBlockOptions options = {});
            // Preserves the Never Indexed marker for intermediaries that may
            // re-encode sensitive fields on another connection.
            LIKESPROGRAM_HTTP_API Result<std::vector<Http2HpackField>> DecodeFields(const std::uint8_t* data, std::size_t size);
            LIKESPROGRAM_HTTP_API Result<std::vector<Http2HpackField>> DecodeFields(const std::vector<std::uint8_t>& data);

            // Encodes raw string literals. Huffman encoding is optional in
            // HPACK; Decode accepts both raw and Huffman forms.
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> Encode(const std::vector<HttpHeader>& headers, bool useDynamicIndexing = true);
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> EncodeFields(const std::vector<Http2HpackField>& fields);

            // Schedules an RFC 7541 dynamic-table-size update at the start of
            // the next encoded block and immediately applies local eviction.
            LIKESPROGRAM_HTTP_API Result<void> SetEncoderDynamicTableSize(std::size_t bytes);

            LIKESPROGRAM_HTTP_API Http2HpackLimits Limits() const noexcept;
            LIKESPROGRAM_HTTP_API Http2HpackSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
