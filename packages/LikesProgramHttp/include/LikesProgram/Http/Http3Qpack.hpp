#pragma once

#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/HttpHeaderBlock.hpp>
#include <LikesProgram/Core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace LikesProgram {
    namespace Http {
        // Parses one RFC 9204 prefixed integer and reports consumed bytes.
        LIKESPROGRAM_HTTP_API Result<std::pair<std::uint64_t, std::size_t>> ParseHttp3QpackPrefixedInteger(const std::uint8_t* data, std::size_t size, std::uint8_t prefixBits);

        // Builds one prefixed integer while preserving instruction flag bits.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3QpackPrefixedInteger(std::uint64_t value, std::uint8_t prefixBits, std::uint8_t firstBytePrefix = 0);

        struct Http3QpackStringLiteral {
            bool huffmanEncoded = false;
            std::vector<std::uint8_t> bytes;
        };

        // Parses an N-bit QPACK string literal and reports consumed bytes.
        // Huffman payload bytes remain opaque; no Huffman decoder is owned here.
        LIKESPROGRAM_HTTP_API Result<std::pair<Http3QpackStringLiteral, std::size_t>> ParseHttp3QpackStringLiteral(const std::uint8_t* data, std::size_t size, std::uint8_t prefixBits);

        // Builds an N-bit QPACK string literal from raw or already-encoded bytes.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>>
            BuildHttp3QpackStringLiteral(const Http3QpackStringLiteral& literal,
                std::uint8_t prefixBits,
                std::uint8_t firstBytePrefix = 0);

        // Decodes one RFC 7541/9204 Huffman payload with a caller-owned
        // output bound. EOS symbols and invalid terminal padding are rejected.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> DecodeHttp3QpackHuffman(const std::uint8_t* data, std::size_t size, std::size_t maxDecodedBytes = 64 * 1024);

        enum class Http3QpackEncoderInstructionType : std::uint8_t {
            SetDynamicTableCapacity,
            InsertWithNameReference,
            InsertWithLiteralName,
            Duplicate
        };

        struct Http3QpackEncoderInstruction {
            Http3QpackEncoderInstructionType type = Http3QpackEncoderInstructionType::SetDynamicTableCapacity;
            std::uint64_t value = 0;
            bool staticTable = false;
            Http3QpackStringLiteral name;
            Http3QpackStringLiteral fieldValue;
        };

        LIKESPROGRAM_HTTP_API Result<std::pair<Http3QpackEncoderInstruction, std::size_t>> ParseHttp3QpackEncoderInstruction(const std::uint8_t* data, std::size_t size);

        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3QpackEncoderInstruction(const Http3QpackEncoderInstruction& instruction);

        class Http3QpackDynamicTable;

        // Applies one already-parsed encoder instruction to a caller-owned
        // dynamic table. Huffman string values are decoded before insertion.
        LIKESPROGRAM_HTTP_API Result<void> ApplyHttp3QpackEncoderInstruction(const Http3QpackEncoderInstruction& instruction, Http3QpackDynamicTable& dynamicTable);

        enum class Http3QpackDecoderInstructionType : std::uint8_t {
            SectionAcknowledgment,
            StreamCancellation,
            InsertCountIncrement
        };

        struct Http3QpackDecoderInstruction {
            Http3QpackDecoderInstructionType type = Http3QpackDecoderInstructionType::SectionAcknowledgment;
            std::uint64_t value = 0;
        };

        LIKESPROGRAM_HTTP_API Result<std::pair<Http3QpackDecoderInstruction, std::size_t>> ParseHttp3QpackDecoderInstruction(const std::uint8_t* data, std::size_t size);

        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3QpackDecoderInstruction(const Http3QpackDecoderInstruction& instruction);

        class Http3QpackSectionTracker;

        // Applies one already-parsed decoder instruction to a caller-owned
        // section lifecycle tracker.
        LIKESPROGRAM_HTTP_API Result<void> ApplyHttp3QpackDecoderInstruction(const Http3QpackDecoderInstruction& instruction, Http3QpackSectionTracker& sectionTracker);

        struct Http3QpackStreamLimits {
            std::size_t maxBufferedBytes = 64 * 1024;
            std::size_t maxInstructions = 4096;
        };

        struct Http3QpackStreamSnapshot {
            std::size_t bufferedBytes = 0;
            std::size_t appliedInstructions = 0;
            bool terminal = false;
            bool finished = false;
        };

        enum class Http3QpackStreamErrorCode : std::uint64_t {
            DecompressionFailed = 0x200,
            EncoderStreamError = 0x201,
            DecoderStreamError = 0x202
        };

        struct Http3QpackStreamQuicActions {
            std::uint64_t quicErrorCode = 0;
            bool closeConnection = false;
        };

        // Maps a non-OK field-section parse/decode result to caller-owned
        // connection-close intent without emitting QUIC frames.
        LIKESPROGRAM_HTTP_API Result<Http3QpackStreamQuicActions> MapHttp3QpackFieldSectionFailure(const Status& failure);

        // Incremental sans-I/O QPACK encoder stream. The dynamic table is
        // caller-owned; Feed may retain an incomplete instruction until Finish.
        class Http3QpackEncoderStream {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QpackEncoderStream(Http3QpackDynamicTable& dynamicTable, Http3QpackStreamLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3QpackEncoderStream();

            Http3QpackEncoderStream(Http3QpackEncoderStream&&) noexcept;
            Http3QpackEncoderStream& operator=(Http3QpackEncoderStream&&) noexcept;
            Http3QpackEncoderStream(const Http3QpackEncoderStream&) = delete;
            Http3QpackEncoderStream& operator=(const Http3QpackEncoderStream&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::uint8_t* data, std::size_t size);
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API Status LastError() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http3QpackStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API Http3QpackStreamLimits Limits() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        // Incremental sans-I/O QPACK decoder stream. The section tracker is
        // caller-owned; malformed or semantically invalid instructions become
        // terminal until Reset.
        class Http3QpackDecoderStream {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QpackDecoderStream(Http3QpackSectionTracker& sectionTracker, Http3QpackStreamLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3QpackDecoderStream();

            Http3QpackDecoderStream(Http3QpackDecoderStream&&) noexcept;
            Http3QpackDecoderStream& operator=(Http3QpackDecoderStream&&) noexcept;
            Http3QpackDecoderStream(const Http3QpackDecoderStream&) = delete;
            Http3QpackDecoderStream& operator=(const Http3QpackDecoderStream&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> Feed(const std::uint8_t* data, std::size_t size);
            LIKESPROGRAM_HTTP_API Result<void> Finish();
            LIKESPROGRAM_HTTP_API Status LastError() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http3QpackStreamQuicActions> FailureActions() const;
            LIKESPROGRAM_HTTP_API Http3QpackStreamLimits Limits() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackStreamSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        struct Http3QpackFieldSectionPrefix {
            std::uint64_t requiredInsertCount = 0;
            std::uint64_t base = 0;
        };

        // Parses the two-integer QPACK encoded field-section prefix. The
        // dynamic table capacity is expressed in bytes; no field lines or
        // dynamic-table ownership are performed by this helper.
        LIKESPROGRAM_HTTP_API Result<std::pair<Http3QpackFieldSectionPrefix, std::size_t>> ParseHttp3QpackFieldSectionPrefix(
            const std::uint8_t* data, std::size_t size,
            std::size_t maxDynamicTableCapacity,
            std::uint64_t totalNumberOfInserts
        );

        // Builds a QPACK encoded field-section prefix using the configured
        // maximum dynamic-table capacity.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3QpackFieldSectionPrefix(const Http3QpackFieldSectionPrefix& prefix, std::size_t maxDynamicTableCapacity);

        enum class Http3QpackFieldLineType : std::uint8_t {
            Indexed,
            IndexedPostBase,
            LiteralWithNameReference,
            LiteralWithPostBaseNameReference,
            LiteralWithLiteralName
        };

        struct Http3QpackFieldLine {
            Http3QpackFieldLineType type = Http3QpackFieldLineType::Indexed;
            std::uint64_t index = 0;
            bool staticTable = false;
            bool neverIndex = false;
            Http3QpackStringLiteral name;
            Http3QpackStringLiteral value;
        };

        // Parses one QPACK field-line representation. Indices and string
        // flags are preserved, but table lookup and Huffman decoding remain
        // outside this stateless boundary.
        LIKESPROGRAM_HTTP_API Result<std::pair<Http3QpackFieldLine, std::size_t>> ParseHttp3QpackFieldLine(const std::uint8_t* data, std::size_t size);

        // Builds one QPACK field-line representation without owning a table.
        LIKESPROGRAM_HTTP_API Result<std::vector<std::uint8_t>> BuildHttp3QpackFieldLine(const Http3QpackFieldLine& fieldLine);

        struct Http3QpackStaticEntry {
            std::uint64_t index = 0;
            std::string name;
            std::string value;
        };

        struct Http3QpackResolvedField {
            Http3QpackStringLiteral name;
            Http3QpackStringLiteral value;
            bool neverIndex = false;
            bool referencedTable = false;
            bool staticTable = false;
            std::uint64_t referencedIndex = 0;
        };

        LIKESPROGRAM_HTTP_API std::size_t Http3QpackStaticTableSize() noexcept;

        LIKESPROGRAM_HTTP_API Result<Http3QpackStaticEntry> GetHttp3QpackStaticEntry(std::uint64_t index);

        struct Http3QpackDynamicEntry {
            std::uint64_t absoluteIndex = 0;
            std::size_t size = 0;
            std::vector<std::uint8_t> name;
            std::vector<std::uint8_t> value;
        };

        struct Http3QpackDynamicTableSnapshot {
            std::size_t capacity = 0;
            std::size_t bytes = 0;
            std::size_t entryCount = 0;
            std::size_t droppedEntries = 0;
            std::uint64_t insertCount = 0;
        };

        class Http3QpackResourceBudget;

        // Sans-I/O FIFO dynamic table. It does not track outstanding references,
        // blocked sections, Required Insert Count, or Huffman-decoded fields.
        class Http3QpackDynamicTable {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QpackDynamicTable(std::size_t maxCapacity = 4096);
            LIKESPROGRAM_HTTP_API ~Http3QpackDynamicTable();

            LIKESPROGRAM_HTTP_API Http3QpackDynamicTable(Http3QpackDynamicTable&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackDynamicTable& operator=(Http3QpackDynamicTable&&) noexcept;
            Http3QpackDynamicTable(const Http3QpackDynamicTable&) = delete;
            Http3QpackDynamicTable& operator=(const Http3QpackDynamicTable&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> SetCapacity(std::size_t capacity);
            LIKESPROGRAM_HTTP_API Result<std::uint64_t> Insert(const std::vector<std::uint8_t>& name, const std::vector<std::uint8_t>& value);
            LIKESPROGRAM_HTTP_API Result<std::uint64_t> Duplicate(std::uint64_t relativeIndex);
            // The budget is non-owning and must outlive this table. Attach it
            // before setting capacity. While attached, the table exclusively
            // owns the budget's dynamic-table capacity and byte lifecycle.
            LIKESPROGRAM_HTTP_API Result<void> AttachResourceBudget(Http3QpackResourceBudget* budget);
            LIKESPROGRAM_HTTP_API bool HasResourceBudget() const noexcept;
            LIKESPROGRAM_HTTP_API Result<Http3QpackDynamicEntry> GetAbsolute(std::uint64_t absoluteIndex) const;
            LIKESPROGRAM_HTTP_API Result<Http3QpackDynamicEntry> GetRelative(std::uint64_t relativeIndex) const;
            LIKESPROGRAM_HTTP_API std::size_t MaxCapacity() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackDynamicTableSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        // Resolves static or dynamic table indices for one parsed field-line.
        // Huffman strings remain encoded and no table state is mutated.
        LIKESPROGRAM_HTTP_API Result<Http3QpackResolvedField> ResolveHttp3QpackFieldLine(const Http3QpackFieldLine& fieldLine, const Http3QpackFieldSectionPrefix& prefix, const Http3QpackDynamicTable& dynamicTable);

        struct Http3QpackFieldSectionLimits {
            std::size_t maxEncodedBytes = 64 * 1024;
            std::size_t maxFields = 256;
            std::size_t maxResolvedBytes = 64 * 1024;
        };

        struct Http3QpackParsedFieldSection {
            Http3QpackFieldSectionPrefix prefix;
            std::vector<Http3QpackResolvedField> fields;
            std::size_t encodedBytes = 0;
            std::size_t resolvedBytes = 0;
            bool hasHuffman = false;
        };

        // Parses and resolves one complete encoded field section. Huffman
        // values remain encoded; callers must not treat this as decoded text.
        LIKESPROGRAM_HTTP_API Result<Http3QpackParsedFieldSection> ParseHttp3QpackFieldSection(const std::uint8_t* data, std::size_t size, const Http3QpackDynamicTable& dynamicTable, Http3QpackFieldSectionLimits limits = {});

        // Decodes every Huffman name/value in an already parsed field section.
        // The returned resolved-byte count describes decoded bytes and is
        // bounded across the complete section.
        LIKESPROGRAM_HTTP_API Result<Http3QpackParsedFieldSection> DecodeHttp3QpackFieldSection(const Http3QpackParsedFieldSection& section, std::size_t maxDecodedBytes = 64 * 1024);

        // Decodes one parsed QPACK section into the common HTTP header model
        // and applies the existing HTTP/3 pseudo-header/field validation.
        // Compression metadata remains available in the input section.
        LIKESPROGRAM_HTTP_API Result<std::vector<HttpHeader>> DecodeHttp3QpackHeaderBlock(const Http3QpackParsedFieldSection& section, HttpHeaderBlockOptions options = {}, std::size_t maxDecodedBytes = 64 * 1024);

        struct Http3QpackSectionTrackerLimits {
            std::size_t maxOutstandingSections = 256;
            std::size_t maxBlockedStreams = 16;
        };

        struct Http3QpackSectionTrackerSnapshot {
            std::uint64_t knownInsertCount = 0;
            std::size_t outstandingSections = 0;
            std::size_t blockedStreams = 0;
            std::size_t acknowledgedSections = 0;
            std::size_t pendingUnblockedStreams = 0; // 尚未由调度方提取的可运行 stream
        };

        // Tracks caller-owned QPACK field-section lifecycle. It does not own
        // streams, dynamic-table entries, encoder/decoder I/O, or timers.
        class Http3QpackSectionTracker {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QpackSectionTracker(Http3QpackSectionTrackerLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3QpackSectionTracker();

            LIKESPROGRAM_HTTP_API Http3QpackSectionTracker(Http3QpackSectionTracker&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackSectionTracker& operator=(Http3QpackSectionTracker&&) noexcept;
            Http3QpackSectionTracker(const Http3QpackSectionTracker&) = delete;
            Http3QpackSectionTracker& operator=(const Http3QpackSectionTracker&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> SetKnownInsertCount(std::uint64_t insertCount);
            LIKESPROGRAM_HTTP_API Result<void> OpenSection(std::uint64_t streamId,std::uint64_t requiredInsertCount);
            LIKESPROGRAM_HTTP_API Result<void> AcknowledgeSection(std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API Result<void> CancelStream(std::uint64_t streamId);
            // The budget is non-owning and must outlive this tracker. Attach it
            // before opening sections. While attached, the tracker exclusively
            // owns the budget's blocked-stream lifecycle; callers may still use
            // the independent dynamic-table and header-block accounting APIs.
            LIKESPROGRAM_HTTP_API Result<void> AttachResourceBudget(Http3QpackResourceBudget* budget);
            LIKESPROGRAM_HTTP_API bool HasResourceBudget() const noexcept;
            // Takes stream ids that became runnable since the previous call.
            // The ascending batch is a local scheduler handoff, not QUIC I/O.
            LIKESPROGRAM_HTTP_API Result<std::vector<std::uint64_t>> TakeUnblockedStreams();

            LIKESPROGRAM_HTTP_API Http3QpackSectionTrackerLimits Limits() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackSectionTrackerSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };

        struct Http3QpackLimits {
            std::size_t maxDynamicTableBytes = 4096;
            std::size_t maxBlockedStreams = 16;
            std::size_t maxHeaderBlockBytes = 64 * 1024;
        };

        struct Http3QpackResourceSnapshot {
            std::size_t dynamicTableCapacity = 0;
            std::size_t dynamicTableBytes = 0;
            std::size_t blockedStreams = 0;
            std::size_t maxBlockedStreams = 0;
        };

        // Sans-I/O QPACK resource accounting. This class does not decode
        // field instructions, Huffman data, or own a control/request stream.
        class Http3QpackResourceBudget {
        public:
            LIKESPROGRAM_HTTP_API explicit Http3QpackResourceBudget(Http3QpackLimits limits = {});
            LIKESPROGRAM_HTTP_API ~Http3QpackResourceBudget();

            LIKESPROGRAM_HTTP_API Http3QpackResourceBudget(Http3QpackResourceBudget&&) noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackResourceBudget& operator=(Http3QpackResourceBudget&&) noexcept;
            Http3QpackResourceBudget(const Http3QpackResourceBudget&) = delete;
            Http3QpackResourceBudget& operator=(const Http3QpackResourceBudget&) = delete;

            LIKESPROGRAM_HTTP_API Result<void> SetDynamicTableCapacity(std::size_t capacity);
            LIKESPROGRAM_HTTP_API Result<void> ReserveDynamicTable(std::size_t bytes);
            LIKESPROGRAM_HTTP_API Result<void> ReleaseDynamicTable(std::size_t bytes);
            LIKESPROGRAM_HTTP_API Result<void> ValidateHeaderBlock(std::size_t bytes) const;
            LIKESPROGRAM_HTTP_API Result<void> OpenBlockedStream(std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API Result<void> CloseBlockedStream(std::uint64_t streamId);
            LIKESPROGRAM_HTTP_API bool IsBlockedStreamOpen(std::uint64_t streamId) const noexcept;

            LIKESPROGRAM_HTTP_API Http3QpackLimits Limits() const noexcept;
            LIKESPROGRAM_HTTP_API Http3QpackResourceSnapshot Snapshot() const noexcept;
            LIKESPROGRAM_HTTP_API void Reset() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> m_impl;
        };
    }
}
