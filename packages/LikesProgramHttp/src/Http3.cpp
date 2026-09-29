#include <LikesProgram/Http/Http3.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace {
    constexpr std::uint64_t kHttp3MaxVarInt = (std::uint64_t{ 1 } << 62) - 1; // QUIC varint 上限

    // 根据首字节前缀返回变长整数宽度。
    std::size_t VarIntWidth(std::uint8_t firstByte) noexcept {
        return std::size_t{ 1 } << (firstByte >> 6);
    }

    // 返回指定宽度能承载的最大无符号值。
    std::uint64_t VarIntLimit(std::size_t width) noexcept {
        switch (width) {
        case 1: return (std::uint64_t{ 1 } << 6) - 1;
        case 2: return (std::uint64_t{ 1 } << 14) - 1;
        case 4: return (std::uint64_t{ 1 } << 30) - 1;
        default: return kHttp3MaxVarInt;
        }
    }

    // 为值选择最短合法编码宽度。
    std::size_t SelectVarIntWidth(std::uint64_t value) noexcept {
        if (value <= VarIntLimit(1)) return 1;
        if (value <= VarIntLimit(2)) return 2;
        if (value <= VarIntLimit(4)) return 4;
        return 8;
    }
}

namespace LikesProgram {
    namespace Http {
        Result<std::pair<std::uint64_t, std::size_t>> ParseHttp3VarInt(
            const std::uint8_t* data,
            std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"HTTP/3 varint is empty");
            }

            const std::size_t width = VarIntWidth(data[0]); // 前缀决定完整编码宽度
            if (size < width) {
                return Status::InvalidArgument(u"HTTP/3 varint is incomplete");
            }

            std::uint64_t value = data[0] & 0x3F; // 去除两位宽度前缀
            for (std::size_t index = 1; index < width; ++index) {
                value = (value << 8) | data[index];
            }

            return std::make_pair(value, width);
        }

        Result<std::vector<std::uint8_t>> BuildHttp3VarInt(std::uint64_t value) {
            if (value > kHttp3MaxVarInt) {
                return Status::InvalidArgument(u"HTTP/3 varint exceeds 62-bit range");
            }

            const std::size_t width = SelectVarIntWidth(value); // 选择最短编码
            const std::uint8_t prefix = width == 1 ? 0x00
                : width == 2 ? 0x40
                : width == 4 ? 0x80
                : 0xC0;
            std::vector<std::uint8_t> output(width, 0); // 变长整数输出缓冲
            for (std::size_t index = width; index > 0; --index) {
                output[index - 1] = static_cast<std::uint8_t>(value & 0xFF);
                value >>= 8;
            }
            output[0] = static_cast<std::uint8_t>(output[0] | prefix);
            return output;
        }

        Result<Http3Frame> ParseHttp3Frame(const std::uint8_t* data, std::size_t size) {
            if (data == nullptr || size == 0) {
                return Status::InvalidArgument(u"HTTP/3 frame is empty");
            }

            auto type = ParseHttp3VarInt(data, size);
            if (!type.IsOk()) return type.GetStatus();
            if (type.Value().second > size) {
                return Status::InvalidArgument(u"HTTP/3 frame type is incomplete");
            }

            const std::size_t lengthOffset = type.Value().second; // 长度字段起点
            auto length = ParseHttp3VarInt(data + lengthOffset, size - lengthOffset);
            if (!length.IsOk()) return length.GetStatus();

            const std::size_t payloadOffset = lengthOffset + length.Value().second;
            if (length.Value().first > std::numeric_limits<std::size_t>::max()
                || payloadOffset > size
                || length.Value().first != size - payloadOffset) {
                return Status::InvalidArgument(u"HTTP/3 frame payload length mismatch");
            }

            Http3Frame frame; // 解析后的帧对象
            frame.type = type.Value().first;
            frame.payload.assign(data + payloadOffset, data + size);
            return frame;
        }

        Result<std::vector<std::uint8_t>> BuildHttp3Frame(const Http3Frame& frame) {
            auto type = BuildHttp3VarInt(frame.type);
            if (!type.IsOk()) return type.GetStatus();
            auto length = BuildHttp3VarInt(frame.payload.size());
            if (!length.IsOk()) return length.GetStatus();

            std::vector<std::uint8_t> output; // 完整 HTTP/3 帧输出
            output.reserve(type.Value().size() + length.Value().size() + frame.payload.size());
            output.insert(output.end(), type.Value().begin(), type.Value().end());
            output.insert(output.end(), length.Value().begin(), length.Value().end());
            output.insert(output.end(), frame.payload.begin(), frame.payload.end());
            return output;
        }

        Result<std::vector<std::uint8_t>> BuildHttp3RequestStreamBytes(
            const std::vector<std::uint8_t>& qpackHeaders,
            const std::vector<std::vector<std::uint8_t>>& dataChunks,
            Http3RequestStreamEncodeLimits limits) {
            if (qpackHeaders.empty()
                || qpackHeaders.size() > limits.maxHeaderBlockBytes
                || qpackHeaders.size() > limits.maxFramePayloadBytes
                || dataChunks.size() > limits.maxDataFrames) {
                return Status::InvalidArgument(
                    u"HTTP/3 request stream headers or frame count exceeds limits");
            }
            std::size_t bodyBytes = 0;
            for (const auto& chunk : dataChunks) {
                if (chunk.size() > limits.maxFramePayloadBytes
                    || chunk.size() > limits.maxBodyBytes - bodyBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 request stream DATA exceeds limits");
                }
                bodyBytes += chunk.size();
            }

            const auto headersFrame = BuildHttp3Frame(Http3Frame{
                static_cast<std::uint64_t>(Http3FrameType::Headers), qpackHeaders });
            if (!headersFrame.IsOk()) return headersFrame.GetStatus();
            std::vector<std::uint8_t> output = headersFrame.Value();
            for (const auto& chunk : dataChunks) {
                if (chunk.empty()) continue;
                const auto dataFrame = BuildHttp3Frame(Http3Frame{
                    static_cast<std::uint64_t>(Http3FrameType::Data), chunk });
                if (!dataFrame.IsOk()) return dataFrame.GetStatus();
                output.insert(output.end(), dataFrame.Value().begin(), dataFrame.Value().end());
            }
            return output;
        }

        Result<std::vector<std::uint8_t>> BuildHttp3ResponseStreamBytes(
            const std::vector<std::uint8_t>& initialQpackHeaders,
            const std::vector<std::vector<std::uint8_t>>& dataChunks,
            const std::optional<std::vector<std::uint8_t>>& trailingQpackHeaders,
            Http3RequestStreamEncodeLimits limits) {
            if (initialQpackHeaders.empty()
                || initialQpackHeaders.size() > limits.maxHeaderBlockBytes
                || initialQpackHeaders.size() > limits.maxFramePayloadBytes
                || (trailingQpackHeaders.has_value()
                    && (trailingQpackHeaders->empty()
                        || trailingQpackHeaders->size() > limits.maxHeaderBlockBytes
                        || trailingQpackHeaders->size() > limits.maxFramePayloadBytes))
                || dataChunks.size() > limits.maxDataFrames
                || (trailingQpackHeaders.has_value()
                    && dataChunks.size() == limits.maxDataFrames)) {
                return Status::InvalidArgument(
                    u"HTTP/3 response stream headers or frame count exceeds limits");
            }
            std::size_t bodyBytes = 0;
            for (const auto& chunk : dataChunks) {
                if (chunk.size() > limits.maxFramePayloadBytes
                    || chunk.size() > limits.maxBodyBytes - bodyBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 response stream DATA exceeds limits");
                }
                bodyBytes += chunk.size();
            }

            const auto headersFrame = BuildHttp3Frame(Http3Frame{
                static_cast<std::uint64_t>(Http3FrameType::Headers), initialQpackHeaders });
            if (!headersFrame.IsOk()) return headersFrame.GetStatus();
            std::vector<std::uint8_t> output = headersFrame.Value();
            for (const auto& chunk : dataChunks) {
                if (chunk.empty()) continue;
                const auto dataFrame = BuildHttp3Frame(Http3Frame{
                    static_cast<std::uint64_t>(Http3FrameType::Data), chunk });
                if (!dataFrame.IsOk()) return dataFrame.GetStatus();
                output.insert(output.end(), dataFrame.Value().begin(), dataFrame.Value().end());
            }
            if (trailingQpackHeaders.has_value()) {
                const auto trailerFrame = BuildHttp3Frame(Http3Frame{
                    static_cast<std::uint64_t>(Http3FrameType::Headers), *trailingQpackHeaders });
                if (!trailerFrame.IsOk()) return trailerFrame.GetStatus();
                output.insert(output.end(), trailerFrame.Value().begin(), trailerFrame.Value().end());
            }
            return output;
        }

        Result<Http3RequestStreamQuicActions> MapHttp3RequestStreamError(
            Http3ErrorCode errorCode) {
            switch (errorCode) {
            case Http3ErrorCode::GeneralProtocolError:
            case Http3ErrorCode::InternalError:
            case Http3ErrorCode::StreamCreationError:
            case Http3ErrorCode::FrameUnexpected:
            case Http3ErrorCode::ClosedCriticalStream:
            case Http3ErrorCode::RequestCancelled:
            case Http3ErrorCode::MessageError:
                return Http3RequestStreamQuicActions{
                    static_cast<std::uint64_t>(errorCode), true, true };
            case Http3ErrorCode::NoError:
                return Status::InvalidArgument(
                    u"HTTP/3 request stream error mapping requires a failure code");
            }
            return Status::InvalidArgument(
                u"HTTP/3 request stream error code is not supported");
        }

        Result<Http3QpackStreamQuicActions> MapHttp3RequestStreamQpackFailure(
            const Status& failure) {
            if (failure.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream QPACK mapping requires a failure status");
            }
            return MapHttp3QpackFieldSectionFailure(failure);
        }

        struct Http3RequestStream::Impl {
            explicit Impl(std::uint64_t expectedStreamId, Http3StreamBodyLimits bodyLimits)
                : streamId(expectedStreamId), limits(bodyLimits) { }

            Result<void> Fail(Http3ErrorCode code, const char16_t* message) {
                if (status.IsOk()) {
                    errorCode = code;
                    status = Status(StatusCode::InvalidArgument, message);
                    state = Http3RequestStreamState::Failed;
                    if (sink != nullptr) {
                        sink->Cancel(HttpBodyCancelReason::ProtocolError);
                    }
                }
                return status;
            }

            Result<void> FailLocal(const Status& failure) {
                if (status.IsOk()) {
                    status = failure;
                    state = Http3RequestStreamState::Failed;
                }
                return status;
            }

            Result<void> Complete() {
                if (sink != nullptr) {
                    auto closed = sink->Close();
                    if (!closed.IsOk()) return FailLocal(closed.GetStatus());
                }
                state = Http3RequestStreamState::Complete;
                return {};
            }

            std::uint64_t streamId = 0;
            Http3StreamBodyLimits limits;
            Http3RequestStreamState state = Http3RequestStreamState::AwaitingHeaders;
            Http3ErrorCode errorCode = Http3ErrorCode::NoError;
            Status status;
            std::vector<std::vector<std::uint8_t>> headers;
            std::vector<std::vector<std::uint8_t>> trailers;
            std::vector<std::uint8_t> body;
            HttpBodySink* sink = nullptr;
            std::size_t bodyBytes = 0;
            std::size_t unknownFrameCount = 0;
        };

        Http3RequestStream::Http3RequestStream(
            std::uint64_t streamId,
            Http3StreamBodyLimits limits)
            : m_impl(std::make_unique<Impl>(streamId, limits)) { }

        Http3RequestStream::~Http3RequestStream() = default;

        Http3RequestStream::Http3RequestStream(Http3RequestStream&&) noexcept = default;

        Http3RequestStream& Http3RequestStream::operator=(
            Http3RequestStream&&) noexcept = default;

        Result<void> Http3RequestStream::Feed(
            const Http3Frame& frame,
            bool endStream) {
            if (!m_impl) return Status::Internal(u"HTTP/3 request stream is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->state == Http3RequestStreamState::Complete) {
                return m_impl->Fail(Http3ErrorCode::FrameUnexpected,
                    u"HTTP/3 request stream received data after FIN");
            }

            const auto type = static_cast<Http3FrameType>(frame.type);
            if (type == Http3FrameType::Settings
                || type == Http3FrameType::Goaway
                || type == Http3FrameType::MaxPushId
                || type == Http3FrameType::CancelPush) {
                return m_impl->Fail(Http3ErrorCode::FrameUnexpected,
                    u"HTTP/3 control frame is not valid on a request stream");
            }

            if (m_impl->state == Http3RequestStreamState::AwaitingHeaders) {
                if (type != Http3FrameType::Headers) {
                    return m_impl->Fail(Http3ErrorCode::FrameUnexpected,
                        u"HTTP/3 request stream must start with HEADERS");
                }
                if (frame.payload.size() > m_impl->limits.maxHeaderBlockBytes) {
                    return m_impl->Fail(Http3ErrorCode::MessageError,
                        u"HTTP/3 request header block exceeds configured limit");
                }
                m_impl->headers.push_back(frame.payload);
                if (endStream) return m_impl->Complete();
                m_impl->state = Http3RequestStreamState::Open;
                return {};
            }

            if (type == Http3FrameType::Data) {
                if (m_impl->state == Http3RequestStreamState::Trailers) {
                    return m_impl->Fail(Http3ErrorCode::MessageError,
                        u"HTTP/3 DATA is not valid after request trailers");
                }
                if (frame.payload.size() > m_impl->limits.maxBodyBytes
                    || m_impl->bodyBytes > m_impl->limits.maxBodyBytes
                    || frame.payload.size() > m_impl->limits.maxBodyBytes
                        - m_impl->bodyBytes) {
                    return m_impl->Fail(Http3ErrorCode::MessageError,
                        u"HTTP/3 request body exceeds configured limit");
                }
                if (m_impl->sink != nullptr) {
                    if (frame.payload.size() > m_impl->sink->WritableBytes()) {
                        return Status(StatusCode::ResourceExhausted,
                            u"HTTP/3 request body sink cannot accept the complete DATA frame");
                    }
                    auto pushed = m_impl->sink->Push(
                        frame.payload.data(), frame.payload.size());
                    if (!pushed.IsOk()) {
                        return m_impl->FailLocal(pushed.GetStatus());
                    }
                    if (pushed.Value() != frame.payload.size()) {
                        return m_impl->FailLocal(Status::Internal(
                            u"HTTP/3 request body sink accepted a partial DATA frame"));
                    }
                }
                else {
                    m_impl->body.insert(m_impl->body.end(),
                        frame.payload.begin(), frame.payload.end());
                }
                m_impl->bodyBytes += frame.payload.size();
                if (endStream) return m_impl->Complete();
                return {};
            }

            if (type == Http3FrameType::Headers) {
                if (m_impl->state == Http3RequestStreamState::Trailers) {
                    return m_impl->Fail(Http3ErrorCode::MessageError,
                        u"HTTP/3 request stream received duplicate trailers");
                }
                if (frame.payload.size() > m_impl->limits.maxTrailerBlockBytes) {
                    return m_impl->Fail(Http3ErrorCode::MessageError,
                        u"HTTP/3 request trailer block exceeds configured limit");
                }
                m_impl->trailers.push_back(frame.payload);
                if (endStream) return m_impl->Complete();
                m_impl->state = Http3RequestStreamState::Trailers;
                return {};
            }

            if (m_impl->unknownFrameCount >= m_impl->limits.maxUnknownFrames) {
                return m_impl->Fail(Http3ErrorCode::MessageError,
                    u"HTTP/3 request stream unknown frame limit exceeded");
            }
            ++m_impl->unknownFrameCount; // 未知扩展帧按 RFC 9114 忽略
            if (endStream) return m_impl->Complete();
            return {};
        }

        Result<void> Http3RequestStream::Finish() {
            if (!m_impl) return Status::Internal(u"HTTP/3 request stream is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->state == Http3RequestStreamState::Complete) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream received a duplicate FIN");
            }
            if (m_impl->state == Http3RequestStreamState::AwaitingHeaders) {
                return m_impl->Fail(Http3ErrorCode::FrameUnexpected,
                    u"HTTP/3 request stream finished before initial HEADERS");
            }
            return m_impl->Complete();
        }

        Result<void> Http3RequestStream::AttachBodySink(HttpBodySink* sink) {
            if (!m_impl) return Status::Internal(u"HTTP/3 request stream is moved-from");
            if (sink == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP/3 request stream body sink is null");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->sink != nullptr) {
                if (m_impl->sink == sink) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream already has a body sink");
            }
            if (m_impl->state != Http3RequestStreamState::AwaitingHeaders
                || !m_impl->headers.empty()
                || !m_impl->trailers.empty()
                || m_impl->bodyBytes != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream body sink must attach before input");
            }
            const auto sinkState = sink->State();
            if ((sinkState != HttpBodySinkState::Open
                    && sinkState != HttpBodySinkState::Paused)
                || sink->BufferedBytes() != 0) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream body sink must be empty and open");
            }
            m_impl->sink = sink;
            return {};
        }

        bool Http3RequestStream::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->sink != nullptr;
        }

        Http3RequestStreamState Http3RequestStream::State() const noexcept {
            return m_impl ? m_impl->state : Http3RequestStreamState::Failed;
        }

        Http3RequestStreamSnapshot Http3RequestStream::Snapshot() const noexcept {
            if (!m_impl) return { Http3RequestStreamState::Failed, 0, 0, 0, 0, 0,
                Http3ErrorCode::InternalError };
            return { m_impl->state, m_impl->streamId, m_impl->headers.size(),
                m_impl->trailers.size(), m_impl->bodyBytes, m_impl->unknownFrameCount,
                m_impl->errorCode };
        }

        const std::vector<std::vector<std::uint8_t>>&
            Http3RequestStream::HeaderBlocks() const noexcept {
            static const std::vector<std::vector<std::uint8_t>> empty;
            return m_impl ? m_impl->headers : empty;
        }

        const std::vector<std::vector<std::uint8_t>>&
            Http3RequestStream::TrailerBlocks() const noexcept {
            static const std::vector<std::vector<std::uint8_t>> empty;
            return m_impl ? m_impl->trailers : empty;
        }

        const std::vector<std::uint8_t>& Http3RequestStream::Body() const noexcept {
            static const std::vector<std::uint8_t> empty;
            return m_impl ? m_impl->body : empty;
        }

        namespace {
            Result<std::vector<HttpHeader>> DecodeStoredQpackHeaderBlock(
                const std::vector<std::uint8_t>& block,
                std::size_t maxEncodedBytes,
                bool trailer,
                const Http3QpackDynamicTable& dynamicTable,
                const Http3QpackResourceBudget* resourceBudget) {
                if (resourceBudget != nullptr) {
                    auto validation = resourceBudget->ValidateHeaderBlock(block.size());
                    if (!validation.IsOk()) return validation.GetStatus();
                }
                auto parsed = ParseHttp3QpackFieldSection(
                    block.data(),
                    block.size(),
                    dynamicTable,
                    Http3QpackFieldSectionLimits{
                        maxEncodedBytes, 256, 64 * 1024 });
                if (!parsed.IsOk()) return parsed.GetStatus();
                return DecodeHttp3QpackHeaderBlock(
                    parsed.Value(), HttpHeaderBlockOptions{ true, trailer });
            }
        }

        Result<std::vector<HttpHeader>> Http3RequestStream::DecodeRequestHeaders(
            const Http3QpackDynamicTable& dynamicTable) const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->headers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no initial HEADERS block");
            }
            return DecodeStoredQpackHeaderBlock(
                m_impl->headers.front(),
                m_impl->limits.maxHeaderBlockBytes,
                false,
                dynamicTable,
                nullptr);
        }

        Result<std::vector<HttpHeader>> Http3RequestStream::DecodeRequestHeaders(
            const Http3QpackDynamicTable& dynamicTable,
            const Http3QpackResourceBudget& resourceBudget) const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->headers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no initial HEADERS block");
            }
            return DecodeStoredQpackHeaderBlock(
                m_impl->headers.front(),
                m_impl->limits.maxHeaderBlockBytes,
                false,
                dynamicTable,
                &resourceBudget);
        }

        Result<std::vector<HttpHeader>> Http3RequestStream::DecodeTrailers(
            const Http3QpackDynamicTable& dynamicTable) const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->trailers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no trailer HEADERS block");
            }
            return DecodeStoredQpackHeaderBlock(
                m_impl->trailers.front(),
                m_impl->limits.maxTrailerBlockBytes,
                true,
                dynamicTable,
                nullptr);
        }

        Result<std::vector<HttpHeader>> Http3RequestStream::DecodeTrailers(
            const Http3QpackDynamicTable& dynamicTable,
            const Http3QpackResourceBudget& resourceBudget) const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->trailers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no trailer HEADERS block");
            }
            return DecodeStoredQpackHeaderBlock(
                m_impl->trailers.front(),
                m_impl->limits.maxTrailerBlockBytes,
                true,
                dynamicTable,
                &resourceBudget);
        }

        Result<void> Http3RequestStream::TrackRequestHeaderSection(
            Http3QpackSectionTracker& sectionTracker,
            const Http3QpackDynamicTable& dynamicTable) const {
            if (!m_impl) {
                return Status::Internal(u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->headers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no initial HEADERS block");
            }
            const auto table = dynamicTable.Snapshot();
            auto prefix = ParseHttp3QpackFieldSectionPrefix(
                m_impl->headers.front().data(),
                m_impl->headers.front().size(),
                table.capacity,
                table.insertCount);
            if (!prefix.IsOk()) return prefix.GetStatus();
            return sectionTracker.OpenSection(
                m_impl->streamId,
                prefix.Value().first.requiredInsertCount);
        }

        Result<void> Http3RequestStream::TrackTrailerSection(
            Http3QpackSectionTracker& sectionTracker,
            const Http3QpackDynamicTable& dynamicTable) const {
            if (!m_impl) {
                return Status::Internal(u"HTTP/3 request stream is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->trailers.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream has no trailer HEADERS block");
            }
            const auto table = dynamicTable.Snapshot();
            auto prefix = ParseHttp3QpackFieldSectionPrefix(
                m_impl->trailers.front().data(),
                m_impl->trailers.front().size(),
                table.capacity,
                table.insertCount);
            if (!prefix.IsOk()) return prefix.GetStatus();
            return sectionTracker.OpenSection(
                m_impl->streamId,
                prefix.Value().first.requiredInsertCount);
        }

        Http3ErrorCode Http3RequestStream::ErrorCode() const noexcept {
            return m_impl ? m_impl->errorCode : Http3ErrorCode::InternalError;
        }

        Status Http3RequestStream::LastError() const {
            return m_impl ? m_impl->status
                : Status::Internal(u"HTTP/3 request stream is moved-from");
        }

        void Http3RequestStream::Reset() noexcept {
            if (!m_impl) return;
            m_impl->state = Http3RequestStreamState::AwaitingHeaders;
            m_impl->errorCode = Http3ErrorCode::NoError;
            m_impl->status = Status::OkStatus();
            m_impl->headers.clear();
            m_impl->trailers.clear();
            m_impl->body.clear();
            m_impl->bodyBytes = 0;
            m_impl->unknownFrameCount = 0;
            if (m_impl->sink != nullptr) m_impl->sink->Reset();
        }

        struct Http3RequestStreamWireDecoder::Impl {
            explicit Impl(std::uint64_t streamId,
                Http3StreamBodyLimits bodyLimits,
                Http3RequestStreamWireLimits wireLimits)
                : stream(streamId, bodyLimits), limits(wireLimits) { }

            Result<void> Fail(const Status& failure, bool cancelSink = true) {
                if (status.IsOk()) {
                    status = failure;
                    if (cancelSink && sink != nullptr) {
                        sink->Cancel(HttpBodyCancelReason::ProtocolError);
                    }
                }
                return status;
            }

            Http3RequestStream stream;
            Http3RequestStreamWireLimits limits;
            std::vector<std::uint8_t> pending;
            std::size_t offset = 0;
            Status status;
            bool finished = false;
            HttpBodySink* sink = nullptr;
            bool bodyBlocked = false;
            bool blockedFin = false;
            std::size_t blockedBodyBytes = 0;

            std::size_t PendingBytes() const noexcept {
                return pending.size() - offset;
            }

            void Compact() {
                if (offset == 0) return;
                if (offset == pending.size()) {
                    pending.clear();
                    offset = 0;
                    return;
                }
                if (offset >= 4096 && offset * 2 >= pending.size()) {
                    pending.erase(pending.begin(), pending.begin() + offset);
                    offset = 0;
                }
            }
        };

        Http3RequestStreamWireDecoder::Http3RequestStreamWireDecoder(
            std::uint64_t streamId,
            Http3StreamBodyLimits bodyLimits,
            Http3RequestStreamWireLimits wireLimits)
            : m_impl(std::make_unique<Impl>(streamId, bodyLimits, wireLimits)) {
            if (wireLimits.maxPendingBytes == 0
                || wireLimits.maxFramePayloadBytes == 0) {
                m_impl->status = Status::InvalidArgument(
                    u"HTTP/3 request stream wire limits must be non-zero");
            }
        }

        Http3RequestStreamWireDecoder::~Http3RequestStreamWireDecoder() = default;

        Http3RequestStreamWireDecoder::Http3RequestStreamWireDecoder(
            Http3RequestStreamWireDecoder&&) noexcept = default;

        Http3RequestStreamWireDecoder& Http3RequestStreamWireDecoder::operator=(
            Http3RequestStreamWireDecoder&&) noexcept = default;

        Result<void> Http3RequestStreamWireDecoder::Feed(
            const std::uint8_t* data,
            std::size_t size,
            bool endStream) {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->finished) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder received data after FIN");
            }
            if (m_impl->bodyBlocked) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder requires an explicit body retry");
            }
            if (data == nullptr && size != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 request stream wire data is null"));
            }
            if (size > m_impl->limits.maxPendingBytes
                - std::min(m_impl->PendingBytes(), m_impl->limits.maxPendingBytes)) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 request stream wire pending limit exceeded"));
            }
            if (size != 0) {
                m_impl->pending.insert(m_impl->pending.end(), data, data + size);
            }

            while (m_impl->PendingBytes() != 0) {
                const auto available = m_impl->PendingBytes();
                const auto* bytes = m_impl->pending.data() + m_impl->offset;
                const auto typeWidth = std::size_t{ 1 } << (bytes[0] >> 6);
                if (available < typeWidth) break;
                auto type = ParseHttp3VarInt(bytes, available);
                if (!type.IsOk()) return m_impl->Fail(type.GetStatus());

                const auto lengthOffset = type.Value().second;
                if (available <= lengthOffset) break;
                const auto lengthWidth = std::size_t{ 1 }
                    << (bytes[lengthOffset] >> 6);
                if (available < lengthOffset + lengthWidth) break;
                auto length = ParseHttp3VarInt(
                    bytes + lengthOffset, available - lengthOffset);
                if (!length.IsOk()) return m_impl->Fail(length.GetStatus());
                if (length.Value().first > m_impl->limits.maxFramePayloadBytes) {
                    return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 request stream wire frame limit exceeded"));
                }

                const auto payloadOffset = lengthOffset + length.Value().second;
                if (payloadOffset > available
                    || length.Value().first > available - payloadOffset) {
                    break;
                }
                const auto frameBytes = payloadOffset
                    + static_cast<std::size_t>(length.Value().first);
                Http3Frame frame;
                frame.type = type.Value().first;
                frame.payload.assign(bytes + payloadOffset,
                    bytes + frameBytes);
                const bool frameFin = endStream && frameBytes == available;
                auto fed = m_impl->stream.Feed(frame, frameFin);
                if (!fed.IsOk()) {
                    if (fed.GetStatus().Code() == StatusCode::ResourceExhausted
                        && m_impl->stream.LastError().IsOk()) {
                        m_impl->bodyBlocked = true;
                        m_impl->blockedFin = endStream;
                        m_impl->blockedBodyBytes = frame.payload.size();
                        return fed;
                    }
                    return m_impl->Fail(
                        fed.GetStatus(), m_impl->stream.LastError().IsOk());
                }
                m_impl->offset += frameBytes;
                m_impl->Compact();
                if (frameFin) m_impl->finished = true;
            }

            if (!endStream) return {};
            if (m_impl->PendingBytes() != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 request stream FIN truncated a frame"));
            }
            if (!m_impl->finished) {
                auto finished = m_impl->stream.Finish();
                if (!finished.IsOk()) return m_impl->Fail(
                    finished.GetStatus(), m_impl->stream.LastError().IsOk());
                m_impl->finished = true;
            }
            return {};
        }

        Result<void> Http3RequestStreamWireDecoder::Finish() {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->bodyBlocked) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder body retry is pending");
            }
            if (m_impl->finished) return {};
            if (m_impl->PendingBytes() != 0) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 request stream FIN truncated a frame"));
            }
            auto finished = m_impl->stream.Finish();
            if (!finished.IsOk()) return m_impl->Fail(
                finished.GetStatus(), m_impl->stream.LastError().IsOk());
            m_impl->finished = true;
            return {};
        }

        Result<void> Http3RequestStreamWireDecoder::AttachBodySink(
            HttpBodySink* sink) {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            if (sink == nullptr) {
                return Status::InvalidArgument(
                    u"HTTP/3 request stream wire body sink is null");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->sink != nullptr) {
                if (m_impl->sink == sink) return {};
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder already has a body sink");
            }
            if (m_impl->PendingBytes() != 0 || m_impl->finished) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire body sink must attach before input");
            }
            auto attached = m_impl->stream.AttachBodySink(sink);
            if (!attached.IsOk()) return attached;
            m_impl->sink = sink;
            return {};
        }

        bool Http3RequestStreamWireDecoder::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->sink != nullptr;
        }

        Result<void> Http3RequestStreamWireDecoder::RetryPendingBody() {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (!m_impl->bodyBlocked) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder has no pending body retry");
            }
            const auto endStream = m_impl->blockedFin;
            m_impl->bodyBlocked = false;
            m_impl->blockedFin = false;
            m_impl->blockedBodyBytes = 0;
            return Feed(nullptr, 0, endStream);
        }

        bool Http3RequestStreamWireDecoder::HasPendingBodyRetry() const noexcept {
            return m_impl != nullptr && m_impl->bodyBlocked;
        }

        std::size_t Http3RequestStreamWireDecoder::PendingBodyBytes() const noexcept {
            return m_impl ? m_impl->blockedBodyBytes : 0;
        }

        Http3RequestStreamWireLimits
            Http3RequestStreamWireDecoder::WireLimits() const noexcept {
            return m_impl ? m_impl->limits : Http3RequestStreamWireLimits{};
        }

        std::size_t Http3RequestStreamWireDecoder::PendingBytes() const noexcept {
            return m_impl ? m_impl->PendingBytes() : 0;
        }

        bool Http3RequestStreamWireDecoder::IsComplete() const noexcept {
            return m_impl != nullptr && m_impl->finished
                && m_impl->status.IsOk();
        }

        Http3RequestStreamSnapshot Http3RequestStreamWireDecoder::Snapshot() const noexcept {
            return m_impl ? m_impl->stream.Snapshot()
                : Http3RequestStreamSnapshot{ Http3RequestStreamState::Failed, 0,
                    0, 0, 0, 0, Http3ErrorCode::InternalError };
        }

        Status Http3RequestStreamWireDecoder::LastError() const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            return m_impl->status.IsOk() ? m_impl->stream.LastError()
                : m_impl->status;
        }

        Result<Http3RequestStreamQuicActions>
            Http3RequestStreamWireDecoder::FailureActions() const {
            if (!m_impl) {
                return Status::Internal(
                    u"HTTP/3 request stream wire decoder is moved-from");
            }
            const auto failure = LastError();
            if (failure.IsOk()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 request stream wire decoder has no failure");
            }
            auto errorCode = Snapshot().errorCode;
            if (errorCode == Http3ErrorCode::NoError) {
                errorCode = Http3ErrorCode::MessageError;
            }
            return MapHttp3RequestStreamError(errorCode);
        }

        const std::vector<std::vector<std::uint8_t>>&
            Http3RequestStreamWireDecoder::HeaderBlocks() const noexcept {
            static const std::vector<std::vector<std::uint8_t>> empty;
            return m_impl ? m_impl->stream.HeaderBlocks() : empty;
        }

        const std::vector<std::vector<std::uint8_t>>&
            Http3RequestStreamWireDecoder::TrailerBlocks() const noexcept {
            static const std::vector<std::vector<std::uint8_t>> empty;
            return m_impl ? m_impl->stream.TrailerBlocks() : empty;
        }

        const std::vector<std::uint8_t>&
            Http3RequestStreamWireDecoder::Body() const noexcept {
            static const std::vector<std::uint8_t> empty;
            return m_impl ? m_impl->stream.Body() : empty;
        }

        void Http3RequestStreamWireDecoder::Reset() noexcept {
            if (!m_impl) return;
            m_impl->stream.Reset();
            m_impl->pending.clear();
            m_impl->offset = 0;
            m_impl->status = Status::OkStatus();
            m_impl->finished = false;
            m_impl->bodyBlocked = false;
            m_impl->blockedFin = false;
            m_impl->blockedBodyBytes = 0;
        }

        struct Http3StreamBodyDecoder::Impl {
            explicit Impl(std::uint64_t expectedStreamId, Http3StreamBodyLimits bodyLimits)
                : streamId(expectedStreamId), limits(bodyLimits) { }

            Result<void> Fail(const Status& failure) {
                if (status.IsOk()) status = failure;
                return status;
            }

            std::uint64_t streamId = 0;
            Http3StreamBodyLimits limits;
            std::vector<std::uint8_t> body;
            HttpBodySink* sink = nullptr;
            std::size_t receivedBodyBytes = 0;
            Status status;
            bool complete = false;
        };

        Http3StreamBodyDecoder::Http3StreamBodyDecoder(
            std::uint64_t streamId,
            Http3StreamBodyLimits limits)
            : m_impl(std::make_unique<Impl>(streamId, limits)) { }

        Http3StreamBodyDecoder::~Http3StreamBodyDecoder() = default;

        Http3StreamBodyDecoder::Http3StreamBodyDecoder(
            Http3StreamBodyDecoder&&) noexcept = default;

        Http3StreamBodyDecoder& Http3StreamBodyDecoder::operator=(
            Http3StreamBodyDecoder&&) noexcept = default;

        Result<void> Http3StreamBodyDecoder::AttachBodySink(HttpBodySink* sink) {
            if (!m_impl) return Status::Internal(u"HTTP/3 stream decoder is moved-from");
            if (sink == nullptr) return Status::InvalidArgument(u"HTTP/3 body sink is null");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->complete || !m_impl->body.empty()) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 body sink must be attached before DATA");
            }
            m_impl->sink = sink;
            return {};
        }

        bool Http3StreamBodyDecoder::HasBodySink() const noexcept {
            return m_impl != nullptr && m_impl->sink != nullptr;
        }

        Result<void> Http3StreamBodyDecoder::Feed(
            std::uint64_t streamId,
            const Http3Frame& frame,
            bool endStream) {
            if (!m_impl) return Status::Internal(u"HTTP/3 stream decoder is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (m_impl->complete) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 DATA frame arrived after stream FIN"));
            }
            if (frame.type != static_cast<std::uint64_t>(Http3FrameType::Data)) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 stream decoder accepts DATA frames only"));
            }
            if (streamId != m_impl->streamId) {
                return m_impl->Fail(Status::InvalidArgument(
                    u"HTTP/3 DATA frame stream id mismatch"));
            }
            if (frame.payload.size() > m_impl->limits.maxBodyBytes
                || m_impl->receivedBodyBytes > m_impl->limits.maxBodyBytes
                || frame.payload.size() > m_impl->limits.maxBodyBytes
                    - m_impl->receivedBodyBytes) {
                return m_impl->Fail(Status(StatusCode::ResourceExhausted,
                    u"HTTP/3 stream body exceeds configured limit"));
            }

            if (m_impl->sink != nullptr) {
                if (frame.payload.size() > m_impl->sink->WritableBytes()) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 body sink cannot accept the complete DATA frame");
                }
                auto pushed = m_impl->sink->Push(frame.payload.data(), frame.payload.size());
                if (!pushed.IsOk()) return m_impl->Fail(pushed.GetStatus());
                if (pushed.Value() != frame.payload.size()) {
                    return Status(StatusCode::ResourceExhausted,
                        u"HTTP/3 body sink accepted only part of the DATA frame");
                }
            } else {
                m_impl->body.insert(m_impl->body.end(), frame.payload.begin(), frame.payload.end());
            }
            m_impl->receivedBodyBytes += frame.payload.size();
            if (endStream) {
                if (m_impl->sink != nullptr) {
                    auto closed = m_impl->sink->Close();
                    if (!closed.IsOk()) return m_impl->Fail(closed.GetStatus());
                }
                m_impl->complete = true;
            }
            return {};
        }

        Result<void> Http3StreamBodyDecoder::Finish() const {
            if (!m_impl) return Status::Internal(u"HTTP/3 stream decoder is moved-from");
            if (!m_impl->status.IsOk()) return m_impl->status;
            if (!m_impl->complete) {
                return Status(StatusCode::FailedPrecondition,
                    u"HTTP/3 stream has not received stream FIN");
            }
            return {};
        }

        void Http3StreamBodyDecoder::Cancel() noexcept {
            Cancel(HttpBodyCancelReason::Application);
        }

        void Http3StreamBodyDecoder::Cancel(HttpBodyCancelReason reason) noexcept {
            if (m_impl && m_impl->status.IsOk() && !m_impl->complete) {
                if (m_impl->sink != nullptr) m_impl->sink->Cancel(reason);
                m_impl->status = Status(StatusCode::Cancelled,
                    u"HTTP/3 stream body was cancelled");
            }
        }

        bool Http3StreamBodyDecoder::IsComplete() const noexcept {
            return m_impl != nullptr && m_impl->complete && m_impl->status.IsOk();
        }

        bool Http3StreamBodyDecoder::IsCancelled() const noexcept {
            return m_impl != nullptr && m_impl->status.Code() == StatusCode::Cancelled;
        }

        const std::vector<std::uint8_t>& Http3StreamBodyDecoder::Body() const noexcept {
            static const std::vector<std::uint8_t> empty;
            return m_impl ? m_impl->body : empty;
        }

        void Http3StreamBodyDecoder::Reset() noexcept {
            if (!m_impl) return;
            if (m_impl->sink != nullptr) m_impl->sink->Reset();
            m_impl->body.clear();
            m_impl->receivedBodyBytes = 0;
            m_impl->status = Status::OkStatus();
            m_impl->complete = false;
        }

        const char* Http3FrameTypeName(std::uint64_t type) noexcept {
            switch (static_cast<Http3FrameType>(type)) {
            case Http3FrameType::Data: return "DATA";
            case Http3FrameType::Headers: return "HEADERS";
            case Http3FrameType::CancelPush: return "CANCEL_PUSH";
            case Http3FrameType::Settings: return "SETTINGS";
            case Http3FrameType::PushPromise: return "PUSH_PROMISE";
            case Http3FrameType::Goaway: return "GOAWAY";
            case Http3FrameType::MaxPushId: return "MAX_PUSH_ID";
            case Http3FrameType::WebTransport: return "WEBTRANSPORT";
            }
            return "UNKNOWN";
        }
    }
}
