#include <LikesProgram/Http/Http2Hpack.hpp>
#include <LikesProgram/Http/Http3Qpack.hpp>

#include <array>
#include <deque>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace {
    using LikesProgram::Http::HttpHeader;

    struct StaticEntry {
        std::string_view name;
        std::string_view value;
    };

    constexpr std::array<StaticEntry, 61> kStaticTable{{
        { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" },
        { ":path", "/" }, { ":path", "/index.html" }, { ":scheme", "http" },
        { ":scheme", "https" }, { ":status", "200" }, { ":status", "204" },
        { ":status", "206" }, { ":status", "304" }, { ":status", "400" },
        { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" },
        { "accept-encoding", "gzip, deflate" }, { "accept-language", "" },
        { "accept-ranges", "" }, { "accept", "" },
        { "access-control-allow-origin", "" }, { "age", "" }, { "allow", "" },
        { "authorization", "" }, { "cache-control", "" },
        { "content-disposition", "" }, { "content-encoding", "" },
        { "content-language", "" }, { "content-length", "" },
        { "content-location", "" }, { "content-range", "" },
        { "content-type", "" }, { "cookie", "" }, { "date", "" },
        { "etag", "" }, { "expect", "" }, { "expires", "" }, { "from", "" },
        { "host", "" }, { "if-match", "" }, { "if-modified-since", "" },
        { "if-none-match", "" }, { "if-range", "" },
        { "if-unmodified-since", "" }, { "last-modified", "" }, { "link", "" },
        { "location", "" }, { "max-forwards", "" },
        { "proxy-authenticate", "" }, { "proxy-authorization", "" },
        { "range", "" }, { "referer", "" }, { "refresh", "" },
        { "retry-after", "" }, { "server", "" }, { "set-cookie", "" },
        { "strict-transport-security", "" }, { "transfer-encoding", "" },
        { "user-agent", "" }, { "vary", "" }, { "via", "" },
        { "www-authenticate", "" }
    }};

    std::size_t EntrySize(const HttpHeader& header) noexcept {
        constexpr std::size_t overhead = 32;
        if (header.name.size() > std::numeric_limits<std::size_t>::max()
                - header.value.size() - overhead) {
            return std::numeric_limits<std::size_t>::max();
        }
        return header.name.size() + header.value.size() + overhead;
    }

    class DynamicTable {
    public:
        explicit DynamicTable(std::size_t limit) : m_limit(limit) {}

        void SetLimit(std::size_t limit) {
            m_limit = limit;
            Evict();
        }

        void Insert(HttpHeader header) {
            const std::size_t bytes = EntrySize(header);
            if (bytes > m_limit) {
                Clear();
                return;
            }
            m_entries.push_front(std::move(header));
            m_bytes += bytes;
            Evict();
        }

        const HttpHeader* At(std::size_t zeroBased) const noexcept {
            return zeroBased < m_entries.size() ? &m_entries[zeroBased] : nullptr;
        }

        std::size_t FindExact(const HttpHeader& header) const noexcept {
            for (std::size_t i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].name == header.name
                        && m_entries[i].value == header.value) return i;
            }
            return m_entries.size();
        }

        std::size_t FindName(std::string_view name) const noexcept {
            for (std::size_t i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].name == name) return i;
            }
            return m_entries.size();
        }

        void Clear() noexcept {
            m_entries.clear();
            m_bytes = 0;
        }

        std::size_t Bytes() const noexcept { return m_bytes; }
        std::size_t Size() const noexcept { return m_entries.size(); }
        std::size_t Limit() const noexcept { return m_limit; }

    private:
        void Evict() {
            while (m_bytes > m_limit && !m_entries.empty()) {
                m_bytes -= EntrySize(m_entries.back());
                m_entries.pop_back();
            }
        }

        std::deque<HttpHeader> m_entries;
        std::size_t m_bytes = 0;
        std::size_t m_limit = 0;
    };

    LikesProgram::Result<std::pair<std::uint64_t, std::size_t>> ParseInteger(
        const std::uint8_t* data, std::size_t size, std::uint8_t prefixBits) {
        auto parsed = LikesProgram::Http::ParseHttp3QpackPrefixedInteger(
            data, size, prefixBits);
        if (!parsed.IsOk()) {
            return LikesProgram::Status(parsed.GetStatus().Code(),
                u"HTTP/2 HPACK prefixed integer is invalid");
        }
        return parsed;
    }

    LikesProgram::Result<std::vector<std::uint8_t>> BuildInteger(
        std::uint64_t value, std::uint8_t prefixBits, std::uint8_t flags) {
        auto built = LikesProgram::Http::BuildHttp3QpackPrefixedInteger(
            value, prefixBits, flags);
        if (!built.IsOk()) {
            return LikesProgram::Status(built.GetStatus().Code(),
                u"HTTP/2 HPACK prefixed integer cannot be encoded");
        }
        return built;
    }

    LikesProgram::Result<std::pair<std::string, std::size_t>> ParseString(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t maxStringBytes) {
        auto literal = LikesProgram::Http::ParseHttp3QpackStringLiteral(
            data, size, 8);
        if (!literal.IsOk()) {
            return LikesProgram::Status(literal.GetStatus().Code(),
                u"HTTP/2 HPACK string literal is invalid");
        }

        auto parsed = literal.MoveValue();
        std::vector<std::uint8_t> bytes;
        if (parsed.first.huffmanEncoded) {
            auto decoded = LikesProgram::Http::DecodeHttp3QpackHuffman(
                parsed.first.bytes.data(), parsed.first.bytes.size(), maxStringBytes);
            if (!decoded.IsOk()) {
                return LikesProgram::Status(decoded.GetStatus().Code(),
                    u"HTTP/2 HPACK Huffman payload is invalid");
            }
            bytes = decoded.MoveValue();
        }
        else {
            if (parsed.first.bytes.size() > maxStringBytes) {
                return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                    u"HTTP/2 HPACK string exceeds configured limit");
            }
            bytes = std::move(parsed.first.bytes);
        }

        return std::pair<std::string, std::size_t>{
            std::string(bytes.begin(), bytes.end()), parsed.second };
    }

    LikesProgram::Result<std::vector<std::uint8_t>> BuildString(
        std::string_view value) {
        LikesProgram::Http::Http3QpackStringLiteral literal;
        literal.bytes.assign(value.begin(), value.end());
        auto built = LikesProgram::Http::BuildHttp3QpackStringLiteral(literal, 8);
        if (!built.IsOk()) {
            return LikesProgram::Status(built.GetStatus().Code(),
                u"HTTP/2 HPACK string literal cannot be encoded");
        }
        return built;
    }

    const HttpHeader* HeaderAt(const DynamicTable& table,
        std::uint64_t oneBasedIndex, HttpHeader& scratch) {
        if (oneBasedIndex == 0) return nullptr;
        if (oneBasedIndex <= kStaticTable.size()) {
            const auto& entry = kStaticTable[static_cast<std::size_t>(oneBasedIndex - 1)];
            scratch = { std::string(entry.name), std::string(entry.value) };
            return &scratch;
        }
        return table.At(static_cast<std::size_t>(oneBasedIndex - kStaticTable.size() - 1));
    }

    std::uint64_t FindExact(const DynamicTable& table, const HttpHeader& header) {
        for (std::size_t i = 0; i < kStaticTable.size(); ++i) {
            if (kStaticTable[i].name == header.name
                    && kStaticTable[i].value == header.value) return i + 1;
        }
        const std::size_t dynamic = table.FindExact(header);
        return dynamic < table.Size() ? kStaticTable.size() + dynamic + 1 : 0;
    }

    std::uint64_t FindName(const DynamicTable& table, std::string_view name) {
        for (std::size_t i = 0; i < kStaticTable.size(); ++i) {
            if (kStaticTable[i].name == name) return i + 1;
        }
        const std::size_t dynamic = table.FindName(name);
        return dynamic < table.Size() ? kStaticTable.size() + dynamic + 1 : 0;
    }

    LikesProgram::Result<void> AppendField(
        std::vector<LikesProgram::Http::Http2HpackField>& fields,
        HttpHeader header,
        LikesProgram::Http::Http2HpackIndexingPolicy indexing,
        std::size_t& headerBytes,
        const LikesProgram::Http::Http2HpackLimits& limits) {
        if (fields.size() >= limits.maxHeaderCount) {
            return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                u"HTTP/2 HPACK header count exceeds configured limit");
        }
        const std::size_t bytes = EntrySize(header);
        if (bytes == std::numeric_limits<std::size_t>::max()
                || bytes > limits.maxHeaderListBytes - headerBytes) {
            return LikesProgram::Status(LikesProgram::StatusCode::ResourceExhausted,
                u"HTTP/2 HPACK header list exceeds configured limit");
        }
        headerBytes += bytes;
        fields.push_back({ std::move(header), indexing });
        return {};
    }
}

namespace LikesProgram {
    namespace Http {
        Result<Http2HpackFailureActions> MapHttp2HpackFailure(
            const Status& failure) {
            if (failure.IsOk()) {
                return Status::InvalidArgument(
                    u"HTTP/2 HPACK failure mapping requires a non-OK status");
            }
            return Http2HpackFailureActions{};
        }

        struct Http2HpackCodec::Impl {
            explicit Impl(Http2HpackLimits configured)
                : limits(configured), decoder(configured.maxDynamicTableBytes),
                  encoder(configured.maxDynamicTableBytes) {}

            Http2HpackLimits limits;
            DynamicTable decoder;
            DynamicTable encoder;
            bool encoderSizeUpdatePending = false;
        };

        Http2HpackCodec::Http2HpackCodec(Http2HpackLimits limits)
            : m_impl(std::make_unique<Impl>(limits)) {}

        Http2HpackCodec::~Http2HpackCodec() = default;
        Http2HpackCodec::Http2HpackCodec(Http2HpackCodec&&) noexcept = default;
        Http2HpackCodec& Http2HpackCodec::operator=(Http2HpackCodec&&) noexcept = default;

        Result<std::vector<Http2HpackField>> Http2HpackCodec::DecodeFields(
            const std::uint8_t* data, std::size_t size) {
            if (data == nullptr && size != 0) {
                return Status::InvalidArgument(u"HTTP/2 HPACK input is null");
            }

            std::vector<Http2HpackField> fields;
            std::size_t headerBytes = 0;
            std::size_t offset = 0;
            bool sawHeader = false;

            while (offset < size) {
                const std::uint8_t first = data[offset];
                if ((first & 0x80u) != 0) {
                    auto index = ParseInteger(data + offset, size - offset, 7);
                    if (!index.IsOk()) {
                        return index.PropagateFailure<std::vector<Http2HpackField>>();
                    }
                    HttpHeader scratch;
                    const HttpHeader* header = HeaderAt(
                        m_impl->decoder, index.Value().first, scratch);
                    if (header == nullptr) {
                        return Status::InvalidArgument(u"HTTP/2 HPACK indexed field is invalid");
                    }
                    auto appended = AppendField(fields, *header,
                        Http2HpackIndexingPolicy::WithoutIndexing,
                        headerBytes, m_impl->limits);
                    if (!appended.IsOk()) {
                        return Result<std::vector<Http2HpackField>>(appended.GetStatus());
                    }
                    offset += index.Value().second;
                    sawHeader = true;
                    continue;
                }

                if ((first & 0xE0u) == 0x20u) {
                    if (sawHeader) {
                        return Status::InvalidArgument(
                            u"HTTP/2 HPACK table update must precede header fields");
                    }
                    auto update = ParseInteger(data + offset, size - offset, 5);
                    if (!update.IsOk()) {
                        return update.PropagateFailure<std::vector<Http2HpackField>>();
                    }
                    if (update.Value().first > m_impl->limits.maxDynamicTableBytes) {
                        return Status::InvalidArgument(
                            u"HTTP/2 HPACK table update exceeds configured maximum");
                    }
                    m_impl->decoder.SetLimit(static_cast<std::size_t>(update.Value().first));
                    offset += update.Value().second;
                    continue;
                }

                const bool incremental = (first & 0xC0u) == 0x40u;
                const bool neverIndexed = !incremental && (first & 0xF0u) == 0x10u;
                const std::uint8_t prefixBits = incremental ? 6 : 4;
                auto nameIndex = ParseInteger(data + offset, size - offset, prefixBits);
                if (!nameIndex.IsOk()) {
                    return nameIndex.PropagateFailure<std::vector<Http2HpackField>>();
                }
                offset += nameIndex.Value().second;

                HttpHeader header;
                if (nameIndex.Value().first == 0) {
                    auto name = ParseString(
                        data + offset, size - offset, m_impl->limits.maxStringBytes);
                    if (!name.IsOk()) {
                        return name.PropagateFailure<std::vector<Http2HpackField>>();
                    }
                    header.name = std::move(name.Value().first);
                    offset += name.Value().second;
                    if (header.name.empty()) {
                        return Status::InvalidArgument(u"HTTP/2 HPACK header name is empty");
                    }
                }
                else {
                    HttpHeader scratch;
                    const HttpHeader* indexed = HeaderAt(
                        m_impl->decoder, nameIndex.Value().first, scratch);
                    if (indexed == nullptr) {
                        return Status::InvalidArgument(u"HTTP/2 HPACK name index is invalid");
                    }
                    header.name = indexed->name;
                }

                auto value = ParseString(
                    data + offset, size - offset, m_impl->limits.maxStringBytes);
                if (!value.IsOk()) {
                    return value.PropagateFailure<std::vector<Http2HpackField>>();
                }
                header.value = std::move(value.Value().first);
                offset += value.Value().second;

                const auto policy = incremental
                    ? Http2HpackIndexingPolicy::Incremental
                    : (neverIndexed ? Http2HpackIndexingPolicy::NeverIndexed
                                    : Http2HpackIndexingPolicy::WithoutIndexing);
                auto appended = AppendField(
                    fields, header, policy, headerBytes, m_impl->limits);
                if (!appended.IsOk()) {
                    return Result<std::vector<Http2HpackField>>(appended.GetStatus());
                }
                if (incremental) m_impl->decoder.Insert(std::move(header));
                sawHeader = true;
            }
            return fields;
        }

        Result<std::vector<Http2HpackField>> Http2HpackCodec::DecodeFields(
            const std::vector<std::uint8_t>& data) {
            return DecodeFields(data.data(), data.size());
        }

        Result<std::vector<HttpHeader>> Http2HpackCodec::Decode(
            const std::uint8_t* data, std::size_t size) {
            auto decoded = DecodeFields(data, size);
            if (!decoded.IsOk()) {
                return decoded.PropagateFailure<std::vector<HttpHeader>>();
            }
            std::vector<HttpHeader> headers;
            headers.reserve(decoded.Value().size());
            for (auto& field : decoded.Value()) {
                headers.push_back(std::move(field.header));
            }
            return headers;
        }

        Result<std::vector<HttpHeader>> Http2HpackCodec::Decode(
            const std::vector<std::uint8_t>& data) {
            return Decode(data.data(), data.size());
        }

        Result<std::vector<HttpHeader>> Http2HpackCodec::DecodeHeaderBlock(
            const std::uint8_t* data,
            std::size_t size,
            HttpHeaderBlockOptions options) {
            auto decoded = Decode(data, size);
            if (!decoded.IsOk()) return decoded;
            const auto validation = ValidateHttp2HeaderBlock(decoded.Value(), options);
            if (!validation.IsOk()) {
                return Result<std::vector<HttpHeader>>(validation.GetStatus());
            }
            return decoded;
        }

        Result<std::vector<HttpHeader>> Http2HpackCodec::DecodeHeaderBlock(
            const std::vector<std::uint8_t>& data,
            HttpHeaderBlockOptions options) {
            return DecodeHeaderBlock(data.data(), data.size(), options);
        }

        Result<std::vector<std::uint8_t>> Http2HpackCodec::Encode(
            const std::vector<HttpHeader>& headers, bool useDynamicIndexing) {
            std::vector<Http2HpackField> fields;
            fields.reserve(headers.size());
            for (const auto& header : headers) {
                fields.push_back({ header, useDynamicIndexing
                    ? Http2HpackIndexingPolicy::Incremental
                    : Http2HpackIndexingPolicy::WithoutIndexing });
            }
            return EncodeFields(fields);
        }

        Result<std::vector<std::uint8_t>> Http2HpackCodec::EncodeFields(
            const std::vector<Http2HpackField>& fields) {
            if (fields.size() > m_impl->limits.maxHeaderCount) {
                return Status(StatusCode::ResourceExhausted,
                    u"HTTP/2 HPACK header count exceeds configured limit");
            }

            std::size_t headerBytes = 0;
            std::vector<std::uint8_t> output;
            if (m_impl->encoderSizeUpdatePending) {
                auto update = BuildInteger(m_impl->encoder.Limit(), 5, 0x20);
                if (!update.IsOk()) return update;
                output.insert(output.end(), update.Value().begin(), update.Value().end());
                m_impl->encoderSizeUpdatePending = false;
            }

            for (const auto& field : fields) {
                const auto& header = field.header;
                if (header.name.empty()
                        || header.name.size() > m_impl->limits.maxStringBytes
                        || header.value.size() > m_impl->limits.maxStringBytes) {
                    return Status::InvalidArgument(
                        u"HTTP/2 HPACK header name/value exceeds configured limits");
                }
                const std::size_t bytes = EntrySize(header);
                if (bytes == std::numeric_limits<std::size_t>::max()
                        || bytes > m_impl->limits.maxHeaderListBytes - headerBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/2 HPACK header list exceeds configured limit");
                }
                headerBytes += bytes;

                const std::uint64_t exact = FindExact(m_impl->encoder, header);
                if (exact != 0
                        && field.indexing != Http2HpackIndexingPolicy::NeverIndexed) {
                    auto encoded = BuildInteger(exact, 7, 0x80);
                    if (!encoded.IsOk()) return encoded;
                    output.insert(output.end(), encoded.Value().begin(), encoded.Value().end());
                    continue;
                }

                const bool index = field.indexing == Http2HpackIndexingPolicy::Incremental
                    && bytes <= m_impl->encoder.Limit();
                const std::uint64_t nameIndex = FindName(m_impl->encoder, header.name);
                const std::uint8_t flags = index ? 0x40
                    : (field.indexing == Http2HpackIndexingPolicy::NeverIndexed
                        ? 0x10 : 0x00);
                auto prefix = BuildInteger(nameIndex, index ? 6 : 4, flags);
                if (!prefix.IsOk()) return prefix;
                output.insert(output.end(), prefix.Value().begin(), prefix.Value().end());
                if (nameIndex == 0) {
                    auto name = BuildString(header.name);
                    if (!name.IsOk()) return name;
                    output.insert(output.end(), name.Value().begin(), name.Value().end());
                }
                auto value = BuildString(header.value);
                if (!value.IsOk()) return value;
                output.insert(output.end(), value.Value().begin(), value.Value().end());
                if (index) m_impl->encoder.Insert(header);
            }
            return output;
        }

        Result<void> Http2HpackCodec::SetEncoderDynamicTableSize(std::size_t bytes) {
            if (bytes > m_impl->limits.maxDynamicTableBytes) {
                return Status::InvalidArgument(
                    u"HTTP/2 HPACK encoder table size exceeds configured maximum");
            }
            m_impl->encoder.SetLimit(bytes);
            m_impl->encoderSizeUpdatePending = true;
            return {};
        }

        Http2HpackLimits Http2HpackCodec::Limits() const noexcept {
            return m_impl->limits;
        }

        Http2HpackSnapshot Http2HpackCodec::Snapshot() const noexcept {
            return {
                m_impl->decoder.Bytes(), m_impl->decoder.Size(), m_impl->decoder.Limit(),
                m_impl->encoder.Bytes(), m_impl->encoder.Size(), m_impl->encoder.Limit()
            };
        }

        void Http2HpackCodec::Reset() noexcept {
            m_impl->decoder.Clear();
            m_impl->decoder.SetLimit(m_impl->limits.maxDynamicTableBytes);
            m_impl->encoder.Clear();
            m_impl->encoder.SetLimit(m_impl->limits.maxDynamicTableBytes);
            m_impl->encoderSizeUpdatePending = false;
        }
    }
}
