#include <LikesProgram/Http/Http.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <LikesProgram/Core/time/Clock.hpp>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    // Http 包工业级回归覆盖三代 codec、无网络 Session 和稳定性边界。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    template<typename T>
    void RequireStatus(const LikesProgram::Result<T>& result,
        LikesProgram::StatusCode code,
        const char* message) {
        Require(!result.IsOk() && result.GetStatus().Code() == code, message);
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Http::PackageName();       // 包身份名称
        const char* packageVersion = LikesProgram::Http::PackageVersion(); // 包统一版本

        Require(LikesProgram::Http::PackageAvailable(), "Http package should be available");
        if (packageName == nullptr) throw std::runtime_error("Http package name should not be null");
        if (packageVersion == nullptr) throw std::runtime_error("Http package version should not be null");
        Require(std::strcmp(packageName, "LikesProgramHttp") == 0, "Http package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Http package version should follow Core version");
    }

    void TestHttp1RequestRoundTrip() {
        LikesProgram::Http::HttpRequest request;          // 组装用请求报文
        request.method = "POST";
        request.target = "/api/orders?q=1";
        request.headers.push_back({ "Host", "example.test" });
        request.headers.push_back({ "Content-Type", "application/octet-stream" });
        request.body = { 'o', 0, 'k', 0xFF };

        auto built = LikesProgram::Http::BuildHttp1Request(request);
        Require(built.IsOk(), "HTTP/1 request should build");
        Require(built.Value().find("Content-Length: 4\r\n") != std::string::npos,
            "HTTP/1 request builder should add Content-Length");

        auto parsed = LikesProgram::Http::ParseHttp1Request(built.Value());
        Require(parsed.IsOk(), "HTTP/1 request should parse after build");
        Require(parsed.Value().method == "POST", "HTTP/1 request method mismatch");
        Require(parsed.Value().target == "/api/orders?q=1", "HTTP/1 request target mismatch");
        Require(LikesProgram::Http::HttpHeaderValue(parsed.Value().headers, "host") == "example.test",
            "HTTP/1 request header lookup should be case insensitive");
        Require(parsed.Value().body == request.body, "HTTP/1 request binary body mismatch");
    }

    void TestHttp1ResponseRoundTrip() {
        LikesProgram::Http::HttpResponse response;         // 组装用响应报文
        response.statusCode = 201;
        response.reason = "Created";
        response.headers.push_back({ "Server", "LikesProgram" });
        response.body = { '{', '}', '\n' };

        auto built = LikesProgram::Http::BuildHttp1Response(response);
        Require(built.IsOk(), "HTTP/1 response should build");
        Require(built.Value().find("HTTP/1.1 201 Created\r\n") == 0,
            "HTTP/1 response status line mismatch");

        auto parsed = LikesProgram::Http::ParseHttp1Response(built.Value());
        Require(parsed.IsOk(), "HTTP/1 response should parse after build");
        Require(parsed.Value().statusCode == 201, "HTTP/1 response status mismatch");
        Require(parsed.Value().reason == "Created", "HTTP/1 response reason mismatch");
        Require(parsed.Value().body == response.body, "HTTP/1 response body mismatch");

        const auto noReason = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 204\r\nX-Trim:\t value \t\r\n\r\n"); // 空原因短语和 OWS 裁剪
        Require(noReason.IsOk() && noReason.Value().reason.empty(),
            "HTTP/1 response should allow an empty reason phrase");
        Require(LikesProgram::Http::HttpHeaderValue(noReason.Value().headers, "x-trim") == "value",
            "HTTP/1 header values should trim optional whitespace");

        const auto headResponse = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n",
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ true, false });
        Require(headResponse.IsOk() && headResponse.Value().body.empty(),
            "HEAD response should accept a hypothetical Content-Length without a body");
        Require(!LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody",
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ true, false }).IsOk(),
            "HEAD response should reject bytes after the header section");
        Require(!LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 204 No Content\r\n\r\nbody").IsOk(),
            "204 response should reject a message body");
        Require(!LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n").IsOk(),
            "204 response should reject framing headers");
        const auto notModified = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 304 Not Modified\r\nContent-Length: 12\r\n\r\n");
        Require(notModified.IsOk() && notModified.Value().body.empty(),
            "304 response should allow hypothetical representation framing without a body");
        const auto notModifiedChunked = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n");
        Require(notModifiedChunked.IsOk() && notModifiedChunked.Value().body.empty(),
            "304 response should allow hypothetical chunked framing without a body");
        const auto connectResponse = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 200 Connection Established\r\n\r\n",
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ false, true });
        Require(connectResponse.IsOk() && connectResponse.Value().body.empty(),
            "successful CONNECT response should end HTTP framing at headers");
        const auto connectFraming = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 200 Connection Established\r\nContent-Length: 0\r\n\r\n",
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ false, true });
        Require(connectFraming.IsOk() && connectFraming.Value().body.empty(),
            "successful CONNECT response should ignore HTTP framing headers");
    }

    void TestHttp1Errors() {
        const auto missingTerminator = LikesProgram::Http::ParseHttp1Request(
            "GET / HTTP/1.1\r\nHost: example.test"); // 缺少头部结束空行
        Require(!missingTerminator.IsOk(), "HTTP/1 parser should require header terminator");

        const auto badLength = LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nabc"); // 正文长度短于声明
        Require(!badLength.IsOk(), "HTTP/1 parser should reject body length mismatch");

        const auto duplicateLength = LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
        Require(!duplicateLength.IsOk(), "HTTP/1 parser should reject duplicate Content-Length");

        const auto transferEncoding = LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
        Require(transferEncoding.IsOk(), "HTTP/1 parser should accept chunked Transfer-Encoding");

        const auto unframedBody = LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\n\r\nabc");
        Require(!unframedBody.IsOk(), "HTTP/1 parser should reject request body without Content-Length");

        const auto invalidName = LikesProgram::Http::ParseHttp1Request(
            "GET / HTTP/1.1\r\nBad(Name: value\r\n\r\n");
        Require(!invalidName.IsOk(), "HTTP/1 parser should reject non-token header names");

        LikesProgram::Http::HttpRequest injected;           // 组装错误路径请求
        injected.method = "GET";
        injected.target = "/";
        injected.headers.push_back({ "X-Test", "ok\r\nBad: yes" });
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject header value injection");

        injected.headers.clear();
        injected.headers.push_back({ "X-Test", std::string("ok\0bad", 6) });
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject control bytes in header values");

        injected.headers = { { "Content-Length", "9" } };
        injected.body = { 'o', 'k' };
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject mismatched Content-Length");

        injected.headers = { { "Transfer-Encoding", "chunked" } };
        Require(LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should accept chunked Transfer-Encoding");

        injected.headers.clear();
        injected.method = "GE(T";
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject invalid method token");

        injected.method = "GET";
        injected.target = "/bad target";
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject whitespace in request target");

        injected.target = "/";
        injected.version = "HTTP/2.0";
        Require(!LikesProgram::Http::BuildHttp1Request(injected).IsOk(),
            "HTTP/1 builder should reject non-HTTP/1 version");

        const auto malformed = LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 nope\r\n\r\n");                      // 非数字状态码
        Require(!malformed.IsOk(), "HTTP/1 parser should reject malformed status line");

        LikesProgram::Http::HttpResponse invalidResponse;   // 响应字段控制字符错误路径
        invalidResponse.reason = std::string("OK\0bad", 6);
        Require(!LikesProgram::Http::BuildHttp1Response(invalidResponse).IsOk(),
            "HTTP/1 builder should reject control bytes in reason phrase");

        LikesProgram::Http::HttpResponse noContent;
        noContent.statusCode = 204;
        noContent.reason = "No Content";
        noContent.body = { 'x' };
        Require(!LikesProgram::Http::BuildHttp1Response(noContent).IsOk(),
            "HTTP/1 builder should reject a 204 body");

        LikesProgram::Http::HttpResponse notModified;
        notModified.statusCode = 304;
        notModified.reason = "Not Modified";
        notModified.headers = { { "Content-Length", "12" } };
        const auto notModifiedWire = LikesProgram::Http::BuildHttp1Response(notModified);
        Require(notModifiedWire.IsOk() && notModifiedWire.Value().ends_with("\r\n\r\n"),
            "304 response builder should preserve hypothetical Content-Length without a body");

        LikesProgram::Http::HttpResponse head;
        head.statusCode = 200;
        head.reason = "OK";
        head.body = { 'h', 'e', 'a', 'd' };
        const auto headWire = LikesProgram::Http::BuildHttp1Response(
            head,
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ true, false });
        Require(headWire.IsOk()
            && headWire.Value().find("Content-Length: 4\r\n") != std::string::npos
            && headWire.Value().ends_with("\r\n\r\n"),
            "HEAD response builder should emit hypothetical length without body bytes");

        LikesProgram::Http::HttpResponse connect;
        connect.statusCode = 200;
        connect.reason = "Connection Established";
        connect.body = { 'x' };
        Require(!LikesProgram::Http::BuildHttp1Response(
            connect,
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ false, true }).IsOk(),
            "successful CONNECT response builder should reject an HTTP body");

        LikesProgram::Http::HttpResponse headChunked;
        headChunked.statusCode = 200;
        headChunked.reason = "OK";
        headChunked.headers = { { "Transfer-Encoding", "chunked" } };
        headChunked.body = { 'h' };
        const auto headChunkedWire = LikesProgram::Http::BuildHttp1Response(
            headChunked,
            LikesProgram::Http::Http1MessageLimits{},
            LikesProgram::Http::Http1ResponseContext{ true, false });
        Require(headChunkedWire.IsOk() && headChunkedWire.Value().ends_with("\r\n\r\n"),
            "HEAD response builder should preserve hypothetical Transfer-Encoding without chunks");
    }

    void TestHttpBodySink() {
        struct BackpressureRecorder final
            : LikesProgram::Http::HttpBodyBackpressureAdapter {
            int pauseCount = 0;
            int resumeCount = 0;

            void Pause() noexcept override { ++pauseCount; }
            void Resume() noexcept override { ++resumeCount; }
        };

        struct DrainRecorder final
            : LikesProgram::Http::HttpBodyDrainObserver {
            std::vector<LikesProgram::Http::HttpBodyDrainEvent> events;
            LikesProgram::Http::HttpBodySink* sink = nullptr;
            bool committedStateObserved = true;

            void Observe(
                const LikesProgram::Http::HttpBodyDrainEvent& event) noexcept override {
                events.push_back(event);
                if (sink != nullptr) {
                    committedStateObserved = committedStateObserved
                        && sink->PulledBytes() == event.totalPulledBytes
                        && sink->BufferedBytes() == event.remainingBufferedBytes;
                }
            }
        };

        LikesProgram::Http::HttpBodySinkLimits limits;
        limits.maxBufferedBytes = 8;
        limits.lowWatermark = 2;
        limits.highWatermark = 6;
        LikesProgram::Http::HttpBodySink sink(limits);
        const std::vector<std::uint8_t> bytes = { 1, 2, 3, 4, 5 };
        BackpressureRecorder backpressure;
        BackpressureRecorder otherBackpressure;
        DrainRecorder drain;
        DrainRecorder otherDrain;
        drain.sink = &sink;

        RequireStatus(sink.Pull(0), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should reject zero-size pulls");
        RequireStatus(sink.AttachDrainObserver(nullptr, 7),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should reject a null drain observer");
        Require(sink.AttachDrainObserver(&drain, 7).IsOk()
                && sink.AttachDrainObserver(&drain, 7).IsOk()
                && sink.HasDrainObserver()
                && sink.DrainObserverStreamId() == 7,
            "HTTP body sink should attach an idempotent drain observer");
        RequireStatus(sink.AttachDrainObserver(&otherDrain, 7),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should reject a conflicting drain observer");
        RequireStatus(sink.AttachDrainObserver(&drain, 8),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should reject a conflicting drain stream id");
        RequireStatus(sink.AttachBackpressure(nullptr), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should reject a null backpressure adapter");
        Require(sink.AttachBackpressure(&backpressure).IsOk()
                && sink.AttachBackpressure(&backpressure).IsOk()
                && sink.HasBackpressure(),
            "HTTP body sink should attach an idempotent backpressure adapter");
        RequireStatus(sink.AttachBackpressure(&otherBackpressure),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should reject a conflicting backpressure adapter");
        Require(sink.Push(bytes.data(), bytes.size()).IsOk(),
            "HTTP body sink should accept an initial chunk");
        Require(backpressure.pauseCount == 0,
            "HTTP body sink should not pause before reaching its high watermark");
        auto accepted = sink.Push(bytes.data(), bytes.size());
        Require(accepted.IsOk() && accepted.Value() == 3 && sink.BufferedBytes() == 8,
            "HTTP body sink should return partial acceptance at its hard limit");
        Require(sink.NeedsPause(), "HTTP body sink should expose its high watermark");
        Require(backpressure.pauseCount == 1,
            "HTTP body sink should notify one pause at its high watermark");

        sink.Pause();
        RequireStatus(sink.Push(bytes.data(), 1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should reject writes while paused");
        auto first = sink.Pull(6);
        Require(first.IsOk() && first.Value().size() == 6
                && sink.BufferedBytes() == 2 && sink.PulledBytes() == 6
                && drain.events.size() == 1
                && drain.events.back().streamId == 7
                && drain.events.back().bytes == 6
                && drain.events.back().totalPulledBytes == 6
                && drain.events.back().remainingBufferedBytes == 2
                && drain.committedStateObserved,
            "HTTP body sink should pull bounded data and release capacity");
        Require(sink.CanResume(), "HTTP body sink should expose its low watermark");
        Require(backpressure.resumeCount == 1,
            "HTTP body sink should notify one resume at its low watermark");
        sink.Resume();
        Require(!sink.IsPaused(), "HTTP body sink should resume writes");
        Require(sink.Close().IsOk() && sink.IsClosed(),
            "HTTP body sink should close while retaining buffered data");
        RequireStatus(sink.Push(bytes.data(), 1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should reject writes after close");
        const auto closedPull = sink.Pull(8);
        Require(closedPull.IsOk() && closedPull.Value().size() == 2
                && sink.PulledBytes() == 8
                && drain.events.size() == 2
                && drain.events.back().bytes == 2
                && drain.events.back().totalPulledBytes == 8
                && drain.events.back().remainingBufferedBytes == 0,
            "HTTP body sink should count bytes drained after close");
        const auto emptyPull = sink.Pull(8);
        Require(emptyPull.IsOk() && emptyPull.Value().empty()
                && drain.events.size() == 2,
            "HTTP body sink should not notify for an empty successful pull");

        sink.Reset();
        Require(sink.PulledBytes() == 8
                && sink.HasDrainObserver()
                && sink.DrainObserverStreamId() == 7
                && sink.Push(bytes.data(), bytes.size()).IsOk()
                && sink.Push(bytes.data(), 1).IsOk()
                && backpressure.pauseCount == 2,
            "HTTP body sink Reset should preserve its monotonic pull counter");
        sink.Reset();
        Require(backpressure.resumeCount == 2,
            "HTTP body sink reset should resume a paused adapter exactly once");
        sink.Cancel();
        Require(sink.IsCancelled() && sink.BufferedBytes() == 0
                && drain.events.size() == 2,
            "HTTP body sink cancellation should release buffered data");
        RequireStatus(sink.Pull(1), LikesProgram::StatusCode::Cancelled,
            "HTTP body sink should reject pulls after cancellation");

        sink.Reset();
        RequireStatus(sink.SetDeadline(
            LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now())),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP body sink should enforce an expired deadline");
        Require(sink.IsExpired() && drain.events.size() == 2,
            "HTTP body sink should expose deadline expiry without a drain event");

        LikesProgram::Http::HttpBodySink invalid({ 8, 9, 2 });
        Require(invalid.State() == LikesProgram::Http::HttpBodySinkState::Failed,
            "HTTP body sink should reject inverted watermarks");
        RequireStatus(invalid.Push(bytes.data(), 1), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should preserve invalid-limit diagnostics");

        LikesProgram::Http::HttpBodySink moving({ 4, 1, 4 });
        DrainRecorder movingDrain;
        movingDrain.sink = &moving;
        Require(moving.AttachDrainObserver(&movingDrain, 99).IsOk()
                && moving.Push(bytes.data(), 3).IsOk()
                && moving.Pull(2).Value().size() == 2
                && moving.PulledBytes() == 2
                && movingDrain.events.size() == 1,
            "HTTP body sink move fixture should count public pulls");
        LikesProgram::Http::HttpBodySink live(std::move(moving));
        movingDrain.sink = &live;
        Require(moving.PulledBytes() == 0 && live.PulledBytes() == 2
                && !moving.HasDrainObserver()
                && moving.DrainObserverStreamId() == 0
                && live.HasDrainObserver()
                && live.DrainObserverStreamId() == 99,
            "HTTP body sink move should transfer its pull counter and observer");
        live.Reset();
        Require(live.PulledBytes() == 2
                && live.Push(bytes.data(), 1).IsOk()
                && live.Pull(1).IsOk()
                && movingDrain.events.size() == 2
                && movingDrain.events.back().totalPulledBytes == 3
                && movingDrain.committedStateObserved,
            "HTTP body sink Reset should retain its counter and drain observer");
    }

    void TestHttpBodyProducer() {
        struct BackpressureRecorder final
            : LikesProgram::Http::HttpBodyBackpressureAdapter {
            int pauseCount = 0;
            int resumeCount = 0;

            void Pause() noexcept override { ++pauseCount; }
            void Resume() noexcept override { ++resumeCount; }
        };

        LikesProgram::Http::HttpBodyProducerLimits limits;
        limits.maxBufferedBytes = 8;
        limits.lowWatermark = 2;
        limits.highWatermark = 6;
        LikesProgram::Http::HttpBodyProducer producer(limits);
        const std::vector<std::uint8_t> bytes = { 1, 2, 3, 4, 5 };
        BackpressureRecorder backpressure;

        RequireStatus(producer.Pull(0), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body producer should reject zero-size pulls");
        auto emptyPrepared = producer.PreparePull(2);
        Require(emptyPrepared.IsOk()
                && !emptyPrepared.Value().available
                && emptyPrepared.Value().id == 0
                && emptyPrepared.Value().payload.empty()
                && !producer.HasPreparedPull(),
            "HTTP body producer should report an empty preparation without an id");
        RequireStatus(producer.CommitPull(0), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body producer should reject a zero prepared-pull id");
        Require(producer.AttachBackpressure(&backpressure).IsOk()
                && producer.HasBackpressure(),
            "HTTP body producer should expose the backpressure adapter boundary");
        Require(producer.Push(bytes.data(), bytes.size()).IsOk(),
            "HTTP body producer should accept application bytes");
        auto accepted = producer.Push(bytes.data(), bytes.size());
            Require(accepted.IsOk() && accepted.Value() == 3
                && producer.BufferedBytes() == 8 && producer.NeedsPause(),
            "HTTP body producer should apply bounded partial acceptance");
        Require(backpressure.pauseCount == 1,
            "HTTP body producer should notify one pause at its high watermark");

        producer.Pause();
        RequireStatus(producer.Push(bytes.data(), 1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should stop application writes while paused");
        auto prepared = producer.PreparePull(6);
        Require(prepared.IsOk()
                && prepared.Value().available
                && prepared.Value().id != 0
                && prepared.Value().payload
                    == std::vector<std::uint8_t>({ 1, 2, 3, 4, 5, 1 })
                && producer.HasPreparedPull()
                && producer.PreparedPullBytes() == 6
                && producer.BufferedBytes() == 8,
            "HTTP body producer preparation should retain queued bytes");
        const auto rolledBackId = prepared.Value().id;
        RequireStatus(producer.Pull(1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should reject legacy pulls while one is prepared");
        RequireStatus(producer.PreparePull(1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should allow only one prepared pull");
        RequireStatus(producer.CommitPull(rolledBackId + 1),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should reject a mismatched prepared-pull id");
        Require(producer.RollbackPull(rolledBackId).IsOk()
                && !producer.HasPreparedPull()
                && producer.PreparedPullBytes() == 0
                && producer.BufferedBytes() == 8
                && backpressure.resumeCount == 0,
            "HTTP body producer rollback should preserve bytes and backpressure");
        prepared = producer.PreparePull(6);
        Require(prepared.IsOk()
                && prepared.Value().payload
                    == std::vector<std::uint8_t>({ 1, 2, 3, 4, 5, 1 })
                && prepared.Value().id != rolledBackId
                && producer.CommitPull(prepared.Value().id).IsOk()
                && producer.BufferedBytes() == 2
                && !producer.HasPreparedPull(),
            "HTTP body producer commit should consume the prepared prefix once");
        RequireStatus(producer.CommitPull(prepared.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should reject a duplicate prepared-pull commit");
        Require(producer.CanResume(), "HTTP body producer should expose its low watermark");
        Require(backpressure.resumeCount == 1,
            "HTTP body producer should notify one resume at its low watermark");
        producer.Resume();
        Require(!producer.IsPaused(), "HTTP body producer should resume application writes");
        Require(producer.Close().IsOk() && producer.IsClosed(),
            "HTTP body producer should close while retaining buffered bytes");
        RequireStatus(producer.Push(bytes.data(), 1), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should reject writes after close");
        auto remaining = producer.Pull(8);
        Require(remaining.IsOk() && remaining.Value().size() == 2,
            "HTTP body producer should drain bytes after close");

        producer.Reset();
        Require(producer.Push(bytes.data(), 3).IsOk(),
            "HTTP body producer cancellation fixture should buffer bytes");
        auto cancelledPrepared = producer.PreparePull(2);
        Require(cancelledPrepared.IsOk() && producer.HasPreparedPull(),
            "HTTP body producer cancellation fixture should prepare bytes");
        producer.Cancel();
        Require(producer.IsCancelled() && producer.BufferedBytes() == 0
                && !producer.HasPreparedPull(),
            "HTTP body producer cancellation should release bytes and prepared state");
        RequireStatus(producer.CommitPull(cancelledPrepared.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer cancellation should invalidate the prepared id");
        RequireStatus(producer.Pull(1), LikesProgram::StatusCode::Cancelled,
            "HTTP body producer should reject pulls after cancellation");

        producer.Reset();
        Require(producer.Push(bytes.data(), 3).IsOk()
                && producer.PreparePull(2).IsOk(),
            "HTTP body producer timeout fixture should prepare bytes");
        RequireStatus(producer.SetDeadline(
            LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now())),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP body producer should enforce an expired deadline");
        Require(producer.IsExpired() && !producer.HasPreparedPull(),
            "HTTP body producer timeout should invalidate the prepared id");

        LikesProgram::Http::HttpBodyProducer movable;
        Require(movable.Push(bytes.data(), 3).IsOk(),
            "HTTP body producer move fixture should buffer bytes");
        auto movedPrepared = movable.PreparePull(2);
        LikesProgram::Http::HttpBodyProducer moved(std::move(movable));
        Require(movedPrepared.IsOk()
                && !movable.HasPreparedPull()
                && moved.HasPreparedPull()
                && moved.PreparedPullBytes() == 2
                && moved.CommitPull(movedPrepared.Value().id).IsOk()
                && moved.BufferedBytes() == 1,
            "HTTP body producer move should transfer its prepared pull");
        RequireStatus(movable.PreparePull(1), LikesProgram::StatusCode::Internal,
            "HTTP body producer moved-from preparation should fail explicitly");

        LikesProgram::Http::HttpBodyProducer invalid({ 8, 9, 2 });
        Require(invalid.State() == LikesProgram::Http::HttpBodyProducerState::Failed,
            "HTTP body producer should reject inverted watermarks");
        RequireStatus(invalid.Push(bytes.data(), 1), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body producer should preserve invalid-limit diagnostics");
    }

    void TestHttpBodyBudget() {
        struct BudgetObserverRecorder final
            : LikesProgram::Http::HttpBodyBudgetObserver {
            int pauseConnectionCount = 0;
            int resumeConnectionCount = 0;
            std::vector<std::uint64_t> pausedStreams;
            std::vector<std::uint64_t> resumedStreams;

            void PauseConnection() noexcept override { ++pauseConnectionCount; }
            void ResumeConnection() noexcept override { ++resumeConnectionCount; }
            void PauseStream(std::uint64_t streamId) noexcept override {
                pausedStreams.push_back(streamId);
            }
            void ResumeStream(std::uint64_t streamId) noexcept override {
                resumedStreams.push_back(streamId);
            }
        };

        LikesProgram::Http::HttpBodyBudget invalid({ 8, 9 });
        RequireStatus(invalid.Reserve(1, 1), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body budget should reject stream limits above the connection limit");
        LikesProgram::Http::HttpBodyBudget invalidWatermarks(
            { 8, 4 }, { 6, 6, 1, 4 });
        RequireStatus(invalidWatermarks.Reserve(1, 1),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP body budget should reject watermarks without hysteresis");
        LikesProgram::Http::HttpBodyBudget movedFrom({ 8, 4 });
        LikesProgram::Http::HttpBodyBudget movedTo(std::move(movedFrom));
        Require(movedFrom.Snapshot().reservedBytes == 0
                && movedFrom.LastError().Code() == LikesProgram::StatusCode::Internal
                && movedFrom.Watermarks().connectionHighBytes == 0
                && movedTo.Snapshot().maxConnectionBytes == 8
                && movedTo.Watermarks().connectionLowBytes == 7
                && movedTo.Watermarks().streamLowBytes == 3,
            "HTTP body budget snapshot should handle moved-from state");

        BudgetObserverRecorder observer;
        BudgetObserverRecorder otherObserver;
        LikesProgram::Http::HttpBodyBudget watermarked(
            { 10, 6 }, { 4, 8, 2, 4 });
        RequireStatus(watermarked.AttachObserver(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP body budget should reject a null observer");
        Require(watermarked.AttachObserver(&observer).IsOk()
                && watermarked.AttachObserver(&observer).IsOk()
                && watermarked.HasObserver(),
            "HTTP body budget should attach an idempotent observer");
        RequireStatus(watermarked.AttachObserver(&otherObserver),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body budget should reject a conflicting observer");
        const auto watermarks = watermarked.Watermarks();
        Require(watermarks.connectionLowBytes == 4
                && watermarks.connectionHighBytes == 8
                && watermarks.streamLowBytes == 2
                && watermarks.streamHighBytes == 4,
            "HTTP body budget should expose configured watermarks");
        Require(watermarked.Reserve(1, 4).IsOk()
                && watermarked.Reserve(1, 1).IsOk()
                && observer.pausedStreams == std::vector<std::uint64_t>{ 1 }
                && observer.pauseConnectionCount == 0,
            "HTTP body budget should pause a stream once at its high watermark");
        Require(watermarked.Reserve(2, 3).IsOk()
                && observer.pauseConnectionCount == 1
                && observer.pausedStreams == std::vector<std::uint64_t>{ 1 },
            "HTTP body budget should pause the connection once across streams");
        Require(watermarked.Reserve(2, 1).IsOk()
                && observer.pauseConnectionCount == 1
                && observer.pausedStreams == std::vector<std::uint64_t>{ 1, 2 }
                && watermarked.NeedsPause(1) && watermarked.NeedsPause(2),
            "HTTP body budget should suppress duplicate connection pauses");
        Require(watermarked.Release(1, 3).IsOk()
                && observer.resumedStreams == std::vector<std::uint64_t>{ 1 }
                && observer.resumeConnectionCount == 0,
            "HTTP body budget should resume a stream at its low watermark");
        Require(watermarked.Release(2, 2).IsOk()
                && observer.resumedStreams == std::vector<std::uint64_t>{ 1, 2 }
                && observer.resumeConnectionCount == 1
                && watermarked.CanResume(1) && watermarked.CanResume(2),
            "HTTP body budget should resume the connection at its low watermark");
        Require(watermarked.Release(1, 2).IsOk()
                && watermarked.Release(2, 2).IsOk()
                && watermarked.Reset().IsOk(),
            "HTTP body budget should clear watermarked stream reservations");
        LikesProgram::Http::HttpBodyBudget movedWatermarked(std::move(watermarked));
        Require(!watermarked.HasObserver()
                && movedWatermarked.HasObserver()
                && movedWatermarked.Watermarks().streamHighBytes == 4
                && movedWatermarked.Reserve(3, 4).IsOk()
                && observer.pausedStreams
                    == std::vector<std::uint64_t>{ 1, 2, 3 }
                && movedWatermarked.Release(3, 2).IsOk()
                && observer.resumedStreams
                    == std::vector<std::uint64_t>{ 1, 2, 3 }
                && movedWatermarked.Release(3, 2).IsOk()
                && movedWatermarked.Reset().IsOk(),
            "HTTP body budget should transfer its observer and restart notifications");

        LikesProgram::Http::HttpBodyBudget lateObserver({ 8, 4 });
        Require(lateObserver.Reserve(7, 1).IsOk(),
            "HTTP body budget late-observer fixture should reserve bytes");
        RequireStatus(lateObserver.AttachObserver(&otherObserver),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body budget should require observer attachment before reservations");

        LikesProgram::Http::HttpBodyBudget budget({ 6, 4 });
        auto reserved = budget.Reserve(1, 4);
        Require(reserved.IsOk() && reserved.Value() == 4
                && budget.ReservedBytes() == 4
                && budget.ReservedBytes(1) == 4,
            "HTTP body budget should reserve up to the per-stream limit");
        auto snapshot = budget.Snapshot();
        Require(snapshot.maxConnectionBytes == 6 && snapshot.maxStreamBytes == 4
                && snapshot.reservedBytes == 4 && snapshot.activeStreams == 1,
            "HTTP body budget snapshot should report limits and active streams");
        reserved = budget.Reserve(2, 4);
        Require(reserved.IsOk() && reserved.Value() == 2
                && budget.ReservedBytes() == 6
                && budget.AvailableBytes(2) == 0,
            "HTTP body budget should cap reservations at the connection limit");
        snapshot = budget.Snapshot();
        Require(snapshot.reservedBytes == 6 && snapshot.activeStreams == 2,
            "HTTP body budget snapshot should track cross-stream reservations");
        RequireStatus(budget.Reset(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body budget should not reset while reservations are active");
        RequireStatus(budget.Release(1, 5), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body budget should reject over-release");
        Require(budget.Release(1, 4).IsOk() && budget.Release(2, 2).IsOk()
                && budget.ReservedBytes() == 0,
            "HTTP body budget should release stream reservations exactly");
        snapshot = budget.Snapshot();
        Require(snapshot.reservedBytes == 0 && snapshot.activeStreams == 0,
            "HTTP body budget snapshot should clear after releases");

        struct BudgetBackpressureRecorder final
            : LikesProgram::Http::HttpBodyBackpressureAdapter {
            int pauseCount = 0;
            int resumeCount = 0;

            void Pause() noexcept override { ++pauseCount; }
            void Resume() noexcept override { ++resumeCount; }
        };
        LikesProgram::Http::HttpBodyBudget earlyBudget(
            { 10, 6 }, { 2, 8, 1, 4 });
        LikesProgram::Http::HttpBodySink earlySink({ 8, 1, 8 });
        BudgetBackpressureRecorder earlyBackpressure;
        const std::vector<std::uint8_t> bytes = { 1, 2, 3, 4, 5 };
        Require(earlySink.AttachBudget(&earlyBudget, 11).IsOk()
                && earlySink.AttachBackpressure(&earlyBackpressure).IsOk()
                && earlySink.Push(bytes.data(), 4).IsOk()
                && earlySink.WritableBytes() == 2
                && earlyBackpressure.pauseCount == 1,
            "HTTP body sink should pause at a stream watermark before its hard limit");
        Require(earlySink.Pull(3).IsOk()
                && earlyBackpressure.resumeCount == 1
                && earlyBudget.CanResume(11),
            "HTTP body sink should resume after shared watermarks recover");

        LikesProgram::Http::HttpBodySink sink({ 8, 0, 8 });
        LikesProgram::Http::HttpBodyProducer producer({ 8, 0, 8 });
        BudgetBackpressureRecorder sinkBackpressure;
        BudgetBackpressureRecorder producerBackpressure;
        RequireStatus(sink.AttachBudget(nullptr, 1), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should reject a null budget");
        Require(sink.AttachBudget(&budget, 1).IsOk()
                && producer.AttachBudget(&budget, 2).IsOk()
                && producer.AttachBudget(&budget, 2).IsOk()
                && sink.AttachBackpressure(&sinkBackpressure).IsOk()
                && producer.AttachBackpressure(&producerBackpressure).IsOk()
                && sink.HasBudget() && producer.HasBudget()
                && sink.BudgetStreamId() == 1 && producer.BudgetStreamId() == 2,
            "HTTP body sink and producer should attach a shared budget");
        RequireStatus(producer.AttachBudget(&budget, 3),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body producer should reject a conflicting budget attachment");
        Require(sink.Push(bytes.data(), bytes.size()).IsOk(),
            "HTTP body sink should reserve within the shared budget");
        auto accepted = producer.Push(bytes.data(), bytes.size());
        Require(accepted.IsOk() && accepted.Value() == 2
                && budget.ReservedBytes() == 6 && sink.NeedsPause()
                && producer.WritableBytes() == 0,
            "HTTP body producer should observe connection-level budget backpressure");
        Require(sinkBackpressure.pauseCount == 1
                && producerBackpressure.pauseCount == 1,
            "HTTP body budget exhaustion should notify both attached adapters");
        Require(sink.Pull(2).IsOk() && budget.ReservedBytes() == 4,
            "HTTP body sink pull should release shared budget bytes");
        accepted = producer.Push(bytes.data(), bytes.size());
        Require(accepted.IsOk() && accepted.Value() == 2
                && budget.ReservedBytes(2) == 4,
            "HTTP body producer should observe released connection capacity");
        auto budgetPrepared = producer.PreparePull(2);
        Require(budgetPrepared.IsOk()
                && budgetPrepared.Value().payload
                    == std::vector<std::uint8_t>({ 1, 2 })
                && producer.BufferedBytes() == 4
                && budget.ReservedBytes(2) == 4,
            "HTTP body producer preparation should retain its shared budget");
        Require(producer.RollbackPull(budgetPrepared.Value().id).IsOk()
                && producer.BufferedBytes() == 4
                && budget.ReservedBytes(2) == 4,
            "HTTP body producer rollback should preserve shared-budget accounting");
        budgetPrepared = producer.PreparePull(2);
        Require(budgetPrepared.IsOk()
                && producer.CommitPull(budgetPrepared.Value().id).IsOk()
                && producer.BufferedBytes() == 2
                && budget.ReservedBytes(2) == 2,
            "HTTP body producer commit should release shared-budget bytes");
        RequireStatus(budget.Reset(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body budget should remain protected while queues retain bytes");
        producer.Reset();
        Require(producerBackpressure.resumeCount == 1,
            "HTTP body producer reset should resume after budget backpressure");
        sink.Cancel();
        Require(budget.ReservedBytes() == 0 && budget.Reset().IsOk(),
            "HTTP body sink and producer reset/cancel should return budget capacity");

        LikesProgram::Http::HttpBodySink buffered;
        Require(buffered.Push(bytes.data(), 1).IsOk(),
            "HTTP body sink should accept bytes before a budget attachment attempt");
        RequireStatus(buffered.AttachBudget(&budget, 9),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body sink should require budget attachment before buffering");
    }

    void TestHttpBodyCancellation() {
        struct Recorder final : LikesProgram::Http::HttpBodyCancellationAdapter {
            int cancelCount = 0;
            int resetCount = 0;
            LikesProgram::Http::HttpBodyCancelReason lastReason =
                LikesProgram::Http::HttpBodyCancelReason::Application;

            void Cancel(LikesProgram::Http::HttpBodyCancelReason reason) noexcept override {
                ++cancelCount;
                lastReason = reason;
            }

            void Reset() noexcept override {
                ++resetCount;
            }
        };

        Recorder recorder;
        Recorder other;
        LikesProgram::Http::HttpBodyCancellation bridge;
        RequireStatus(bridge.AttachAdapter(nullptr), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body cancellation should reject a null adapter");
        Require(bridge.AttachAdapter(&recorder).IsOk() && bridge.HasAdapter(),
            "HTTP body cancellation should attach a non-owning adapter");

        LikesProgram::Http::HttpBodySink sink;
        RequireStatus(sink.AttachCancellation(nullptr), LikesProgram::StatusCode::InvalidArgument,
            "HTTP body sink should reject a null cancellation bridge");
        Require(sink.AttachCancellation(&bridge).IsOk()
                && sink.AttachCancellation(&bridge).IsOk()
                && sink.HasCancellation(),
            "HTTP body sink should attach an idempotent cancellation bridge");
        sink.Cancel(LikesProgram::Http::HttpBodyCancelReason::PeerReset);
        Require(sink.IsCancelled() && bridge.IsCancelled()
                && bridge.Reason() == LikesProgram::Http::HttpBodyCancelReason::PeerReset
                && recorder.cancelCount == 1,
            "HTTP body sink cancellation should notify the adapter once");
        sink.Cancel(LikesProgram::Http::HttpBodyCancelReason::ProtocolError);
        Require(recorder.cancelCount == 1,
            "HTTP body cancellation should suppress duplicate adapter actions");
        sink.Reset();
        Require(!bridge.IsCancelled() && recorder.resetCount == 1,
            "HTTP body sink reset should reopen the bridge and notify the adapter");
        RequireStatus(bridge.AttachAdapter(&other), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP body cancellation should reject a conflicting adapter");

        LikesProgram::Http::HttpBodyProducer producer;
        Require(producer.AttachCancellation(&bridge).IsOk() && producer.HasCancellation(),
            "HTTP body producer should attach the shared cancellation bridge");
        RequireStatus(producer.SetDeadline(
            LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now())),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP body producer should report an expired deadline");
        Require(producer.IsExpired() && bridge.IsCancelled()
                && bridge.Reason() == LikesProgram::Http::HttpBodyCancelReason::DeadlineExceeded
                && recorder.cancelCount == 2,
            "HTTP body producer deadline should map to the adapter reason");
        producer.Reset();
        Require(!bridge.IsCancelled() && recorder.resetCount == 2,
            "HTTP body producer reset should release cancellation state");

        bridge.Cancel(LikesProgram::Http::HttpBodyCancelReason::ProtocolError);
        bridge.Cancel(LikesProgram::Http::HttpBodyCancelReason::Application);
        Require(bridge.IsCancelled() && recorder.cancelCount == 3,
            "HTTP body cancellation should preserve the first terminal reason");
        bridge.Reset();
        Require(!bridge.IsCancelled() && recorder.resetCount == 3,
            "HTTP body cancellation reset should reopen after direct cancellation");
    }

    void TestHttp1TransferEncoding() {
        LikesProgram::Http::HttpRequest request;
        request.method = "POST";
        request.target = "/chunked";
        request.headers = { { "Transfer-Encoding", "chunked" } };
        request.body = { 'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a' };
        request.trailers = { { "X-Checksum", "ok" } };

        const auto built = LikesProgram::Http::BuildHttp1Request(request);
        Require(built.IsOk(), "chunked request should build");
        Require(built.Value().find("Transfer-Encoding: chunked\r\n") != std::string::npos,
            "chunked request should preserve transfer coding");
        Require(built.Value().find("Content-Length:") == std::string::npos,
            "chunked request must not include Content-Length");

        const auto parsed = LikesProgram::Http::ParseHttp1Request(built.Value());
        Require(parsed.IsOk() && parsed.Value().body == request.body,
            "chunked request should round trip body");
        Require(parsed.Value().trailers.size() == 1
            && parsed.Value().trailers.front().value == "ok",
            "chunked request should round trip trailers");

        const std::string wire =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\n\r\n"
            "4;foo=bar\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: yes\r\n\r\n";
        const auto response = LikesProgram::Http::ParseHttp1Response(wire);
        Require(response.IsOk() && response.Value().body
            == std::vector<std::uint8_t>{ 'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a' },
            "chunk extensions and response chunks should parse");
        Require(response.Value().trailers.size() == 1
            && response.Value().trailers.front().name == "X-Trailer",
            "response trailers should parse after the zero chunk");

        LikesProgram::Http::Http1ChunkedDecoder decoder;
        for (char byte : "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n") {
            if (byte == '\0') break;
            Require(decoder.Feed(std::string_view(&byte, 1)).IsOk(),
                "chunked decoder should accept one-byte fragments");
        }
        Require(decoder.Finish().IsOk() && decoder.IsComplete()
            && decoder.Body() == request.body,
            "chunked decoder should complete across input fragments");

        const auto conflict = LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n"
            "0\r\n\r\n");
        Require(!conflict.IsOk(), "Transfer-Encoding and Content-Length must conflict");
        Require(!LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n").IsOk(),
            "unsupported transfer coding should be rejected");
        Require(!LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked, chunked\r\n\r\n"
            "0\r\n\r\n").IsOk(),
            "duplicate chunked coding should be rejected");
        Require(!LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\nWikiX\r\n0\r\n\r\n").IsOk(),
            "chunk data without the required CRLF should be rejected");
        Require(!LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
            "0\r\nContent-Length: 1\r\n\r\n").IsOk(),
            "framing headers must not appear in trailers");
        Require(!LikesProgram::Http::ParseHttp1Request(
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
            "0\r\n\r\nextra").IsOk(),
            "bytes after a complete chunked message should be rejected");

        LikesProgram::Http::Http1MessageLimits limits;
        limits.maxBodyBytes = 3;
        Require(!LikesProgram::Http::ParseHttp1Response(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\nWiki\r\n0\r\n\r\n", limits).IsOk(),
            "chunked body limit should be enforced");

        LikesProgram::Http::HttpBodySink sink({ 4, 1, 3 });
        LikesProgram::Http::Http1ChunkedDecoder sinkDecoder;
        Require(sinkDecoder.AttachBodySink(&sink).IsOk() && sinkDecoder.HasBodySink(),
            "HTTP/1 chunked decoder should attach a bounded body sink before data");
        Require(sinkDecoder.Feed("2\r\nok\r\n").IsOk()
            && sink.BufferedBytes() == 2 && sinkDecoder.Body().empty(),
            "HTTP/1 chunked decoder should hand off chunk data without retaining a vector body");
        auto sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'o', 'k' },
            "HTTP/1 body sink should expose handed-off chunk data");
        Require(sinkDecoder.Feed("3\r\nabc\r\n0\r\n\r\n").IsOk()
            && sinkDecoder.IsComplete() && sink.IsClosed(),
            "HTTP/1 chunked decoder should close the sink after trailers");
        sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'a', 'b', 'c' },
            "HTTP/1 closed sink should retain the final chunk for draining");

        LikesProgram::Http::HttpBodySink pausedSink({ 3, 0, 3 });
        pausedSink.Pause();
        LikesProgram::Http::Http1ChunkedDecoder pausedDecoder;
        Require(pausedDecoder.AttachBodySink(&pausedSink).IsOk(),
            "HTTP/1 chunked decoder should attach a paused sink");
        RequireStatus(pausedDecoder.Feed("3\r\nabc\r\n"),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/1 chunked decoder should retain pending data under sink backpressure");
        Require(pausedSink.BufferedBytes() == 0 && !pausedDecoder.IsComplete(),
            "HTTP/1 chunked backpressure should not partially consume a chunk");
        pausedSink.Resume();
        Require(pausedDecoder.Feed({}).IsOk() && pausedSink.BufferedBytes() == 3,
            "HTTP/1 chunked decoder should retry retained data after sink resume");
        Require(pausedDecoder.Feed("0\r\n\r\n").IsOk()
            && pausedDecoder.IsComplete() && pausedSink.IsClosed(),
            "HTTP/1 chunked decoder should finish after a backpressure retry");

        LikesProgram::Http::HttpBodySink cancelledSink;
        LikesProgram::Http::HttpBodyCancellation cancelledBridge;
        LikesProgram::Http::Http1ChunkedDecoder cancelledDecoder;
        Require(cancelledDecoder.AttachBodySink(&cancelledSink).IsOk(),
            "HTTP/1 chunked decoder should attach a cancellable sink");
        Require(cancelledSink.AttachCancellation(&cancelledBridge).IsOk(),
            "HTTP/1 chunked decoder should attach a cancellation bridge");
        cancelledDecoder.Cancel(LikesProgram::Http::HttpBodyCancelReason::PeerReset);
        Require(cancelledDecoder.IsCancelled() && cancelledSink.IsCancelled()
                && cancelledBridge.Reason()
                    == LikesProgram::Http::HttpBodyCancelReason::PeerReset,
            "HTTP/1 chunked cancellation should propagate its reason to the sink");
        RequireStatus(cancelledDecoder.Feed("0\r\n\r\n"),
            LikesProgram::StatusCode::Cancelled,
            "HTTP/1 cancelled decoder should reject further input");
        cancelledDecoder.Reset();
        Require(cancelledSink.State() == LikesProgram::Http::HttpBodySinkState::Open
            && cancelledDecoder.Feed("0\r\n\r\n").IsOk(),
            "HTTP/1 chunked Reset should reset the attached sink and decoder");
    }

    void TestHttp1LargePayload() {
        LikesProgram::Http::HttpResponse response;           // 1 MiB 二进制响应负载
        response.body.resize(1024 * 1024);
        for (std::size_t index = 0; index < response.body.size(); ++index) {
            response.body[index] = static_cast<std::uint8_t>(index & 0xFF);
        }

        const auto built = LikesProgram::Http::BuildHttp1Response(response);
        Require(built.IsOk(), "HTTP/1 should build a 1 MiB payload");
        const auto parsed = LikesProgram::Http::ParseHttp1Response(built.Value());
        Require(parsed.IsOk() && parsed.Value().body == response.body,
            "HTTP/1 should round trip a 1 MiB payload");
    }

    void TestHttp2PrefaceAndFrameRoundTrip() {
        const auto preface = LikesProgram::Http::BuildHttp2ConnectionPreface(); // 连接前言字节
        Require(LikesProgram::Http::IsHttp2ConnectionPreface(preface.data(), preface.size()),
            "HTTP/2 preface should match itself");
        Require(!LikesProgram::Http::IsHttp2ConnectionPreface(nullptr, preface.size()),
            "HTTP/2 preface should reject null input");

        auto alteredPreface = preface;                       // 单字节损坏前言
        alteredPreface.back() ^= 0x1;
        Require(!LikesProgram::Http::IsHttp2ConnectionPreface(
            alteredPreface.data(), alteredPreface.size()),
            "HTTP/2 preface should reject changed bytes");

        LikesProgram::Http::Http2Frame frame;               // 组装用 DATA 帧
        frame.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
        frame.flags = 0x1;
        frame.streamId = 1;
        frame.payload = { 'h', '2' };

        auto built = LikesProgram::Http::BuildHttp2Frame(frame);
        Require(built.IsOk(), "HTTP/2 frame should build");
        Require(built.Value().size() == 11, "HTTP/2 frame size should include header and payload");

        auto parsed = LikesProgram::Http::ParseHttp2Frame(built.Value());
        Require(parsed.IsOk(), "HTTP/2 frame should parse after build");
        Require(parsed.Value().length == 2, "HTTP/2 frame length mismatch");
        Require(parsed.Value().type == frame.type, "HTTP/2 frame type mismatch");
        Require(parsed.Value().flags == frame.flags, "HTTP/2 frame flags mismatch");
        Require(parsed.Value().streamId == 1, "HTTP/2 frame stream id mismatch");
        Require(parsed.Value().payload == frame.payload, "HTTP/2 frame payload mismatch");
        Require(std::string(LikesProgram::Http::Http2FrameTypeName(frame.type)) == "DATA",
            "HTTP/2 frame type name mismatch");
        Require(std::string(LikesProgram::Http::Http2FrameTypeName(0xFE)) == "UNKNOWN",
            "HTTP/2 unknown frame type should be preserved");
    }

    void TestHttp2ErrorsAndLargePayload() {
        const std::vector<std::uint8_t> shortHeader = { 0, 0, 1 }; // 不足 9 字节帧头
        Require(!LikesProgram::Http::ParseHttp2Frame(shortHeader).IsOk(),
            "HTTP/2 parser should reject incomplete header");

        const std::vector<std::uint8_t> lengthMismatch = {
            0, 0, 2, 0, 0, 0, 0, 0, 1, 'x'
        };
        Require(!LikesProgram::Http::ParseHttp2Frame(lengthMismatch).IsOk(),
            "HTTP/2 parser should reject payload length mismatch");

        const std::vector<std::uint8_t> reservedWire = {
            0, 0, 0, 4, 0, 0x80, 0, 0, 1
        };
        const auto reservedParsed = LikesProgram::Http::ParseHttp2Frame(reservedWire);
        Require(reservedParsed.IsOk() && reservedParsed.Value().streamId == 1,
            "HTTP/2 parser should clear the reserved stream id bit");

        LikesProgram::Http::Http2Frame reserved;             // stream id reserved bit 错误路径
        reserved.streamId = 0x80000001U;
        Require(!LikesProgram::Http::BuildHttp2Frame(reserved).IsOk(),
            "HTTP/2 builder should reject reserved stream id bit");

        LikesProgram::Http::Http2Frame large;                // 1 MiB 帧负载
        large.type = 0xFE;
        large.streamId = 0x7FFFFFFF;
        large.payload.resize(1024 * 1024, 0xA5);
        const auto largeBytes = LikesProgram::Http::BuildHttp2Frame(large);
        Require(largeBytes.IsOk(), "HTTP/2 should build a 1 MiB frame");
        const auto largeParsed = LikesProgram::Http::ParseHttp2Frame(largeBytes.Value());
        Require(largeParsed.IsOk() && largeParsed.Value().payload == large.payload,
            "HTTP/2 should round trip a 1 MiB frame");

        LikesProgram::Http::Http2Frame tooLarge;             // 超过 24-bit 长度上限
        tooLarge.payload.resize(0x01000000U);
        Require(!LikesProgram::Http::BuildHttp2Frame(tooLarge).IsOk(),
            "HTTP/2 builder should reject payloads above 24-bit length");
    }

    void TestHttp2SessionStateAndFlowControl() {
        using namespace LikesProgram;
        using namespace LikesProgram::Http;

        const auto applicationCancellation = MapHttp2BodyCancellation(
            HttpBodyCancelReason::Application);
        const auto protocolCancellation = MapHttp2BodyCancellation(
            HttpBodyCancelReason::ProtocolError);
        const auto peerCancellation = MapHttp2BodyCancellation(
            HttpBodyCancelReason::PeerReset);
        Require(applicationCancellation.IsOk()
                && applicationCancellation.Value().kind
                    == Http2BodyCancellationActionKind::ResetStream
                && applicationCancellation.Value().errorCode == Http2ErrorCode::Cancel
                && protocolCancellation.IsOk()
                && protocolCancellation.Value().errorCode == Http2ErrorCode::ProtocolError
                && peerCancellation.IsOk()
                && peerCancellation.Value().kind
                    == Http2BodyCancellationActionKind::AlreadyHandled
                && peerCancellation.Value().errorCode == Http2ErrorCode::NoError,
            "HTTP/2 body cancellation should map to RST_STREAM without echoing peer reset");
        RequireStatus(MapHttp2BodyCancellation(
                static_cast<HttpBodyCancelReason>(0xFF)),
            StatusCode::InvalidArgument,
            "HTTP/2 body cancellation should reject unknown reasons");

        const std::vector<Http2Setting> settings{
            { static_cast<std::uint16_t>(Http2SettingId::InitialWindowSize), 20 },
            { static_cast<std::uint16_t>(Http2SettingId::MaxFrameSize), 16384 }
        };
        const auto settingsBytes = BuildHttp2Settings(settings);
        Require(settingsBytes.IsOk() && settingsBytes.Value().size() == 12,
            "HTTP/2 SETTINGS builder should emit six bytes per setting");
        const auto parsedSettings = ParseHttp2Settings(settingsBytes.Value());
        Require(parsedSettings.IsOk() && parsedSettings.Value().size() == settings.size()
            && parsedSettings.Value()[0].value == 20,
            "HTTP/2 SETTINGS should round trip");
        Require(!BuildHttp2Settings({ settings[0], settings[0] }).IsOk(),
            "HTTP/2 SETTINGS builder should reject duplicate identifiers");
        Require(!ParseHttp2Settings({ 0, 1, 2 }).IsOk(),
            "HTTP/2 SETTINGS parser should reject partial entries");

        Http2Session session(true, Http2SessionLimits{ 10, 2, 16384 });
        const auto initialSnapshot = session.Snapshot();
        Require(initialSnapshot.client
            && initialSnapshot.localLimits.initialWindowSize == 10
            && initialSnapshot.localLimits.maxConcurrentStreams == 2
            && initialSnapshot.connectionSendWindow == 10
            && initialSnapshot.connectionReceiveWindow == 10
            && initialSnapshot.activeLocalStreams == 0
            && initialSnapshot.activeRemoteStreams == 0,
            "HTTP/2 session snapshot should expose initial limits and windows");
        Require(session.OpenLocalStream(1).IsOk(),
            "HTTP/2 client should open an odd local stream");
        Require(session.OpenLocalStream(3).IsOk(),
            "HTTP/2 client should interleave a second local stream");
        Require(session.Snapshot().activeLocalStreams == 2
            && session.Snapshot().activeRemoteStreams == 0,
            "HTTP/2 session snapshot should count active local streams");
        RequireStatus(session.OpenLocalStream(5), StatusCode::ResourceExhausted,
            "HTTP/2 session should enforce peer stream concurrency");
        Require(session.ApplySettings(settings).IsOk() && session.SettingsAckPending(),
            "HTTP/2 session should apply peer settings and require ACK");
        const auto settingsSnapshot = session.Snapshot();
        Require(settingsSnapshot.peerInitialWindow == 20
            && settingsSnapshot.peerMaxConcurrentStreams == 2
            && settingsSnapshot.peerMaxFrameSize == 16384
            && settingsSnapshot.settingsAckPending == 1,
            "HTTP/2 session snapshot should expose peer SETTINGS and ACK count");
        Require(session.ApplySettings({
                { static_cast<std::uint16_t>(Http2SettingId::MaxConcurrentStreams), 3 } }).IsOk()
            && session.SettingsAckPending(),
            "HTTP/2 session should track more than one pending SETTINGS ACK");
        Require(session.Snapshot().peerMaxConcurrentStreams == 3
            && session.Snapshot().settingsAckPending == 2,
            "HTTP/2 session snapshot should refresh peer concurrency and ACK count");
        Require(session.AcknowledgeSettings().IsOk() && session.SettingsAckPending(),
            "HTTP/2 session should expose SETTINGS ACK pending until all ACKs are sent");
        Require(session.AcknowledgeSettings().IsOk() && !session.SettingsAckPending(),
            "HTTP/2 session should clear the final SETTINGS ACK pending");
        RequireStatus(session.AcknowledgeSettings(), StatusCode::FailedPrecondition,
            "HTTP/2 session should reject an unsolicited SETTINGS ACK");

        Require(session.ReceiveHeaders(1, false, false).IsOk(),
            "HTTP/2 session should begin a fragmented header block");
        Require(session.Snapshot().continuationStream == 1,
            "HTTP/2 session snapshot should expose the pending CONTINUATION stream");
        RequireStatus(session.ReceiveData(1, 1, false), StatusCode::InvalidArgument,
            "HTTP/2 session should reject DATA during CONTINUATION");

        Http2Session streamSession(true, Http2SessionLimits{ 10, 4, 16384 });
        Require(streamSession.OpenLocalStream(1).IsOk(),
            "HTTP/2 stream state fixture should open local stream");
        Require(streamSession.ReceiveHeaders(1, false, false).IsOk()
            && streamSession.ContinueHeaders(1, true).IsOk(),
            "HTTP/2 session should close a fragmented header block on END_HEADERS");
        Require(streamSession.ReceiveData(1, 10, false).IsOk()
            && streamSession.ConnectionReceiveWindow() == 0,
            "HTTP/2 session should debit connection receive credit");
        RequireStatus(streamSession.ReceiveData(1, 1, false), StatusCode::ResourceExhausted,
            "HTTP/2 session should reject data over receive credit");

        Http2Session sendSession(true, Http2SessionLimits{ 10, 4, 16384 });
        Require(sendSession.OpenLocalStream(1).IsOk(),
            "HTTP/2 send fixture should open local stream");
        RequireStatus(sendSession.SendData(1, 11), StatusCode::ResourceExhausted,
            "HTTP/2 session should report send flow-control blocking");
        const auto sendErrorContext = sendSession.LastHttpErrorContext();
        Require(sendErrorContext.valid
                && sendErrorContext.version == HttpVersion::Http2
                && sendErrorContext.scope == HttpErrorScope::Stream
                && sendErrorContext.origin == HttpErrorOrigin::Resource
                && sendErrorContext.streamId == 1
                && sendErrorContext.unitKind == HttpErrorUnitKind::Frame
                && sendErrorContext.unitType
                    == static_cast<std::uint64_t>(Http2FrameType::Data)
                && sendErrorContext.statusCode == StatusCode::ResourceExhausted
                && sendSession.LastStatus().Code() == StatusCode::ResourceExhausted,
            "HTTP/2 resource failures should retain version, stream, frame, and status context");
        Require(sendSession.ApplyWindowUpdate(0, 1).IsOk()
            && sendSession.ApplyWindowUpdate(1, 1).IsOk()
            && sendSession.SendData(1, 11).IsOk(),
            "HTTP/2 session should resume after connection and stream WINDOW_UPDATE");
        Require(sendSession.EndStream(1, true).IsOk(),
            "HTTP/2 session should half-close the local side");
        Require(sendSession.ReceiveData(1, 0, true).IsOk(),
            "HTTP/2 session should close the peer side with END_STREAM");
        Require(sendSession.Stream(1).IsOk()
            && sendSession.Stream(1).Value().state == Http2StreamState::Closed,
            "HTTP/2 stream should enter CLOSED after both END_STREAM markers");
        Require(sendSession.Snapshot().activeLocalStreams == 0
            && sendSession.Snapshot().activeRemoteStreams == 0
            && sendSession.Snapshot().connectionSendWindow == 0
            && sendSession.Snapshot().connectionReceiveWindow == 10,
            "HTTP/2 session snapshot should reflect closed streams and windows");

        Http2Session peerSession(false, Http2SessionLimits{ 10, 1, 16384 });
        Require(peerSession.ReceiveHeaders(1, true, true).IsOk(),
            "HTTP/2 server should accept an odd peer stream with a complete request");
        Require(peerSession.SendData(1, 10).IsOk(),
            "HTTP/2 server should send response DATA on a peer-initiated stream");
        RequireStatus(peerSession.ReceiveHeaders(3, false, true), StatusCode::ResourceExhausted,
            "HTTP/2 server should enforce local stream concurrency");
        Require(peerSession.EndStream(1, true).IsOk(),
            "HTTP/2 server should close its response side");
        RequireStatus(peerSession.SendData(1, 0), StatusCode::FailedPrecondition,
            "HTTP/2 server should reject DATA after closing its response side");
        Require(peerSession.ReceiveHeaders(3, false, true).IsOk(),
            "HTTP/2 closed response should release a concurrency slot");

        Http2Session goawaySession(true);
        Require(goawaySession.OpenLocalStream(1).IsOk()
            && goawaySession.SendGoaway(1).IsOk(),
            "HTTP/2 session should enter GOAWAY state");
        Require(goawaySession.State() == Http2SessionState::GoingAway,
            "HTTP/2 GOAWAY should expose GoingAway state");
        Require(goawaySession.Snapshot().state == Http2SessionState::GoingAway
            && goawaySession.Snapshot().localGoawayLastStream == 1,
            "HTTP/2 session snapshot should expose local GOAWAY state");
        RequireStatus(goawaySession.OpenLocalStream(3), StatusCode::FailedPrecondition,
            "HTTP/2 GOAWAY should reject new local streams");
        Require(goawaySession.ReceiveGoaway(0x7FFFFFFFU).IsOk(),
            "HTTP/2 peer GOAWAY should have an independent last-stream-id");
        Require(goawaySession.Snapshot().peerGoawayLastStream == 0x7FFFFFFFU,
            "HTTP/2 session snapshot should expose peer GOAWAY state");

        Http2Session invalidSession(true);
        RequireStatus(invalidSession.ApplySettings({
                { static_cast<std::uint16_t>(Http2SettingId::MaxFrameSize), 1024 } }),
            StatusCode::InvalidArgument,
            "HTTP/2 session should reject an invalid MAX_FRAME_SIZE");
        Require(invalidSession.State() == Http2SessionState::Closed
            && invalidSession.LastError() == Http2ErrorCode::ProtocolError,
            "HTTP/2 protocol errors should close the connection state");
        Require(invalidSession.Snapshot().state == Http2SessionState::Closed
            && invalidSession.Snapshot().lastError == Http2ErrorCode::ProtocolError,
            "HTTP/2 session snapshot should preserve terminal error state");
        const auto settingsErrorContext = invalidSession.LastHttpErrorContext();
        Require(settingsErrorContext.valid
                && settingsErrorContext.version == HttpVersion::Http2
                && settingsErrorContext.scope == HttpErrorScope::Connection
                && settingsErrorContext.origin == HttpErrorOrigin::Protocol
                && settingsErrorContext.unitKind == HttpErrorUnitKind::Frame
                && settingsErrorContext.unitType
                    == static_cast<std::uint64_t>(Http2FrameType::Settings)
                && settingsErrorContext.statusCode == StatusCode::InvalidArgument,
            "HTTP/2 protocol failures should retain connection and SETTINGS coordinates");

        Http2Session movedSnapshotSource(true);
        Http2Session movedSnapshotTarget(std::move(movedSnapshotSource));
        Require(movedSnapshotTarget.Snapshot().state == Http2SessionState::Open,
            "HTTP/2 session snapshot should survive move construction");
        Require(movedSnapshotSource.Snapshot().state == Http2SessionState::Closed
            && movedSnapshotSource.Snapshot().lastError == Http2ErrorCode::InternalError,
            "HTTP/2 moved-from snapshot should report a terminal internal state");
    }

    void TestHttp2StreamBodyDecoder() {
        LikesProgram::Http::Http2StreamBodyDecoder decoder(
            3, LikesProgram::Http::Http2StreamBodyLimits{ 3 });
        RequireStatus(decoder.AttachBodySink(nullptr), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/2 decoder should reject a null body sink");
        RequireStatus(decoder.Finish(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/2 stream should require END_STREAM before Finish");

        LikesProgram::Http::Http2Frame first;
        first.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
        first.streamId = 3;
        first.payload = { 'a', 'b' };
        first.length = static_cast<std::uint32_t>(first.payload.size());
        Require(decoder.Feed(first).IsOk(), "HTTP/2 stream should accept an intermediate DATA frame");
        Require(!decoder.IsComplete() && decoder.Body() == std::vector<std::uint8_t>{ 'a', 'b' },
            "HTTP/2 stream should retain intermediate body bytes");

        LikesProgram::Http::Http2Frame end = first;
        end.flags = 0x1;
        end.payload = { 'c' };
        end.length = 1;
        Require(decoder.Feed(end).IsOk() && decoder.IsComplete(),
            "HTTP/2 stream should complete on END_STREAM");
        Require(decoder.Finish().IsOk()
            && decoder.Body() == std::vector<std::uint8_t>{ 'a', 'b', 'c' },
            "HTTP/2 stream should finish with the concatenated body");
        RequireStatus(decoder.Feed(end), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/2 stream should reject DATA after END_STREAM");

        decoder.Reset();
        LikesProgram::Http::Http2Frame emptyEnd = first;
        emptyEnd.payload.clear();
        emptyEnd.length = 0;
        emptyEnd.flags = 0x1;
        Require(decoder.Feed(emptyEnd).IsOk() && decoder.Body().empty(),
            "HTTP/2 stream should accept an empty END_STREAM frame");

        decoder.Reset();
        LikesProgram::Http::Http2Frame wrongStream = first;
        wrongStream.streamId = 5;
        RequireStatus(decoder.Feed(wrongStream), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/2 stream should reject a different stream id");
        decoder.Reset();
        LikesProgram::Http::Http2Frame wrongType = first;
        wrongType.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Headers);
        RequireStatus(decoder.Feed(wrongType), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/2 stream should reject non-DATA frames");
        decoder.Reset();
        LikesProgram::Http::Http2Frame wrongLength = first;
        wrongLength.length = 1;
        RequireStatus(decoder.Feed(wrongLength), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/2 stream should reject a DATA length mismatch");

        decoder.Reset();
        LikesProgram::Http::Http2Frame overLimit = first;
        overLimit.payload = { 'a', 'b', 'c', 'd' };
        overLimit.length = 4;
        RequireStatus(decoder.Feed(overLimit), LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/2 stream should enforce its body limit");
        decoder.Reset();
        decoder.Cancel();
        Require(decoder.IsCancelled(), "HTTP/2 stream should expose cancellation");
        RequireStatus(decoder.Feed(first), LikesProgram::StatusCode::Cancelled,
            "HTTP/2 cancelled stream should reject further DATA");
        decoder.Reset();
        Require(decoder.Feed(emptyEnd).IsOk(),
            "HTTP/2 stream Reset should permit a new body");

        LikesProgram::Http::HttpBodySink sink({ 4, 1, 3 });
        LikesProgram::Http::Http2StreamBodyDecoder sinkDecoder(11);
        Require(sinkDecoder.AttachBodySink(&sink).IsOk() && sinkDecoder.HasBodySink(),
            "HTTP/2 stream should attach a bounded body sink before DATA");
        LikesProgram::Http::Http2Frame sinkFirst;
        sinkFirst.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
        sinkFirst.streamId = 11;
        sinkFirst.payload = { 'x', 'y' };
        sinkFirst.length = static_cast<std::uint32_t>(sinkFirst.payload.size());
        Require(sinkDecoder.Feed(sinkFirst).IsOk() && sink.BufferedBytes() == 2
            && sinkDecoder.Body().empty(),
            "HTTP/2 sink decoder should hand off DATA without retaining a vector body");
        auto sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'x', 'y' },
            "HTTP/2 body sink should expose handed-off bytes");

        LikesProgram::Http::Http2Frame sinkEnd = sinkFirst;
        sinkEnd.payload = { 'z', '!' };
        sinkEnd.length = static_cast<std::uint32_t>(sinkEnd.payload.size());
        sinkEnd.flags = 0x1;
        Require(sinkDecoder.Feed(sinkEnd).IsOk() && sinkDecoder.IsComplete() && sink.IsClosed(),
            "HTTP/2 sink decoder should close the sink on END_STREAM");
        sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'z', '!' },
            "HTTP/2 closed sink should retain the final DATA frame for draining");

        LikesProgram::Http::HttpBodySink blockedSink({ 2, 0, 2 });
        LikesProgram::Http::HttpBodyCancellation blockedBridge;
        LikesProgram::Http::Http2StreamBodyDecoder blockedDecoder(12);
        Require(blockedDecoder.AttachBodySink(&blockedSink).IsOk(),
            "HTTP/2 stream should attach a second body sink");
        Require(blockedSink.AttachCancellation(&blockedBridge).IsOk(),
            "HTTP/2 stream should attach a cancellation bridge");
        LikesProgram::Http::Http2Frame oversized = sinkFirst;
        oversized.streamId = 12;
        oversized.payload = { '1', '2', '3' };
        oversized.length = static_cast<std::uint32_t>(oversized.payload.size());
        RequireStatus(blockedDecoder.Feed(oversized), LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/2 sink decoder should reject an oversized frame without partial writes");
        Require(blockedSink.BufferedBytes() == 0 && !blockedDecoder.IsCancelled(),
            "HTTP/2 sink backpressure should leave the decoder retryable");
        blockedSink.Reset();
        oversized.payload = { '1', '2' };
        oversized.length = 2;
        Require(blockedDecoder.Feed(oversized).IsOk() && blockedSink.BufferedBytes() == 2,
            "HTTP/2 sink decoder should accept a frame after capacity is available");
        blockedDecoder.Reset();
        Require(blockedSink.State() == LikesProgram::Http::HttpBodySinkState::Open,
            "HTTP/2 decoder Reset should reset the attached sink");
        blockedDecoder.Cancel(LikesProgram::Http::HttpBodyCancelReason::ProtocolError);
        Require(blockedSink.IsCancelled() && blockedDecoder.IsCancelled()
                && blockedBridge.Reason()
                    == LikesProgram::Http::HttpBodyCancelReason::ProtocolError,
            "HTTP/2 decoder cancellation should propagate its reason to the sink");

        LikesProgram::Http::HttpBodySink limitedSink({ 3, 0, 3 });
        LikesProgram::Http::Http2StreamBodyDecoder limitedDecoder(
            13, LikesProgram::Http::Http2StreamBodyLimits{ 3 });
        Require(limitedDecoder.AttachBodySink(&limitedSink).IsOk(),
            "HTTP/2 limited decoder should attach a body sink");
        LikesProgram::Http::Http2Frame limitedFrame = sinkFirst;
        limitedFrame.streamId = 13;
        limitedFrame.payload = { 'a', 'b' };
        limitedFrame.length = 2;
        Require(limitedDecoder.Feed(limitedFrame).IsOk(),
            "HTTP/2 limited decoder should accept body within its cumulative limit");
        Require(limitedSink.Pull(3).IsOk(),
            "HTTP/2 limited decoder test should drain the first sink frame");
        limitedFrame.payload = { 'c', 'd' };
        limitedFrame.length = 2;
        RequireStatus(limitedDecoder.Feed(limitedFrame), LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/2 sink decoder should retain its cumulative stream body limit");
    }

    void TestHttp2HpackCodec() {
        using LikesProgram::Http::Http2HpackCodec;

        // RFC 7541 C.3.1: first request without Huffman coding.
        const std::vector<std::uint8_t> firstBlock{
            0x82, 0x86, 0x84, 0x41, 0x0f,
            'w', 'w', 'w', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm'
        };
        Http2HpackCodec decoder;
        const auto first = decoder.Decode(firstBlock);
        Require(first.IsOk() && first.Value().size() == 4,
            "HPACK first request vector should decode");
        Require(first.Value()[0].name == ":method" && first.Value()[0].value == "GET",
            "HPACK indexed method mismatch");
        Require(first.Value()[3].name == ":authority"
                && first.Value()[3].value == "www.example.com",
            "HPACK literal authority mismatch");
        Http2HpackCodec validatedDecoder;
        Require(validatedDecoder.DecodeHeaderBlock(firstBlock).IsOk(),
            "HPACK request block should integrate with HTTP/2 header validation");
        Require(decoder.Snapshot().decoderDynamicTableEntries == 1,
            "HPACK incremental field should enter the dynamic table");

        // RFC 7541 C.3.2 reuses dynamic index 62 and inserts cache-control.
        const std::vector<std::uint8_t> secondBlock{
            0x82, 0x86, 0x84, 0xbe, 0x58, 0x08,
            'n', 'o', '-', 'c', 'a', 'c', 'h', 'e'
        };
        const auto second = decoder.Decode(secondBlock);
        Require(second.IsOk() && second.Value().size() == 5,
            "HPACK dynamic-table request vector should decode");
        Require(second.Value()[3].value == "www.example.com"
                && second.Value()[4].name == "cache-control"
                && second.Value()[4].value == "no-cache",
            "HPACK dynamic-table fields mismatch");
        Require(decoder.Snapshot().decoderDynamicTableEntries == 2,
            "HPACK dynamic table should retain both request entries");

        // RFC 7541 C.4.1: the same authority encoded with the shared Huffman code.
        const std::vector<std::uint8_t> huffmanBlock{
            0x82, 0x86, 0x84, 0x41, 0x8c,
            0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a,
            0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff
        };
        Http2HpackCodec huffmanDecoder;
        const auto huffman = huffmanDecoder.Decode(huffmanBlock);
        Require(huffman.IsOk() && huffman.Value().size() == 4
                && huffman.Value()[3].value == "www.example.com",
            "HPACK Huffman request vector should decode");

        const std::vector<LikesProgram::Http::HttpHeader> headers{
            { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { ":authority", "example.test" }, { "user-agent", "LikesProgram" }
        };
        Http2HpackCodec encoder;
        const auto encoded = encoder.Encode(headers);
        Require(encoded.IsOk(), "HPACK header block should encode");
        Http2HpackCodec roundTripDecoder;
        const auto roundTrip = roundTripDecoder.Decode(encoded.Value());
        Require(roundTrip.IsOk() && roundTrip.Value().size() == headers.size(),
            "HPACK encoded block should decode");
        for (std::size_t i = 0; i < headers.size(); ++i) {
            Require(roundTrip.Value()[i].name == headers[i].name
                    && roundTrip.Value()[i].value == headers[i].value,
                "HPACK round-trip field mismatch");
        }

        const std::vector<LikesProgram::Http::Http2HpackField> sensitive{
            { { "authorization", "Bearer secret" },
                LikesProgram::Http::Http2HpackIndexingPolicy::NeverIndexed }
        };
        const auto sensitiveBlock = encoder.EncodeFields(sensitive);
        Require(sensitiveBlock.IsOk() && !sensitiveBlock.Value().empty()
                && (sensitiveBlock.Value()[0] & 0xf0u) == 0x10u,
            "HPACK sensitive field should use Never Indexed representation");
        Http2HpackCodec sensitiveDecoder;
        const auto sensitiveFields = sensitiveDecoder.DecodeFields(sensitiveBlock.Value());
        Require(sensitiveFields.IsOk() && sensitiveFields.Value().size() == 1
                && sensitiveFields.Value()[0].indexing
                    == LikesProgram::Http::Http2HpackIndexingPolicy::NeverIndexed
                && sensitiveFields.Value()[0].header.value == "Bearer secret"
                && sensitiveDecoder.Snapshot().decoderDynamicTableEntries == 0,
            "HPACK Never Indexed policy should survive decode without table insertion");

        Require(encoder.SetEncoderDynamicTableSize(64).IsOk(),
            "HPACK encoder table size should update");
        const auto resized = encoder.Encode({ { "x", "y" } });
        Require(resized.IsOk() && !resized.Value().empty()
                && (resized.Value()[0] & 0xe0u) == 0x20u,
            "HPACK table-size update should lead the next block");
        Http2HpackCodec resizedDecoder;
        Require(resizedDecoder.Decode(resized.Value()).IsOk()
                && resizedDecoder.Snapshot().decoderDynamicTableLimit == 64,
            "HPACK decoder should apply a leading table-size update");

        const std::vector<std::uint8_t> lateUpdate{ 0x82, 0x20 };
        RequireStatus(decoder.Decode(lateUpdate), LikesProgram::StatusCode::InvalidArgument,
            "HPACK table-size update after a field should fail");
        const std::vector<std::uint8_t> zeroIndex{ 0x80 };
        RequireStatus(decoder.Decode(zeroIndex), LikesProgram::StatusCode::InvalidArgument,
            "HPACK zero indexed field should fail");

        LikesProgram::Http::Http2HpackLimits tight;
        tight.maxDynamicTableBytes = 32;
        tight.maxHeaderListBytes = 40;
        tight.maxHeaderCount = 1;
        tight.maxStringBytes = 8;
        Http2HpackCodec limited(tight);
        RequireStatus(limited.Decode(firstBlock), LikesProgram::StatusCode::ResourceExhausted,
            "HPACK decoded string limit should be enforced");
        RequireStatus(limited.Encode(headers), LikesProgram::StatusCode::ResourceExhausted,
            "HPACK encoded header-count limit should be enforced");
        const std::vector<std::uint8_t> oversizedUpdate{ 0x3f, 0x02 };
        RequireStatus(limited.Decode(oversizedUpdate), LikesProgram::StatusCode::InvalidArgument,
            "HPACK peer table size above the configured maximum should fail");

        Http2HpackCodec invalidHeaderEncoder;
        const auto invalidHeader = invalidHeaderEncoder.Encode(
            { { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
              { "Uppercase", "rejected" } }, false);
        Require(invalidHeader.IsOk(),
            "HPACK codec should preserve bytes before semantic validation");
        Http2HpackCodec invalidHeaderDecoder;
        RequireStatus(invalidHeaderDecoder.DecodeHeaderBlock(invalidHeader.Value()),
            LikesProgram::StatusCode::InvalidArgument,
            "HPACK integration should reject invalid HTTP/2 header semantics");

        const auto malformed = decoder.Decode(zeroIndex);
        Require(!malformed.IsOk(), "HPACK malformed fixture should fail");
        const auto actions = LikesProgram::Http::MapHttp2HpackFailure(
            malformed.GetStatus());
        Require(actions.IsOk() && actions.Value().closeConnection
                && actions.Value().connectionErrorCode
                    == static_cast<std::uint32_t>(
                        LikesProgram::Http::Http2ErrorCode::CompressionError),
            "HPACK failure should map to connection COMPRESSION_ERROR");
        RequireStatus(LikesProgram::Http::MapHttp2HpackFailure(
                LikesProgram::Status::OkStatus()),
            LikesProgram::StatusCode::InvalidArgument,
            "HPACK success status should not map to a failure action");
    }

    class LoopbackTransport final : public LikesProgram::Http::HttpTransport {
    public:
        // 记录协议选择并返回固定响应，模拟用户自己的网络适配器。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            LikesProgram::Http::HttpVersion version) override {
            lastVersion = version;
            lastTarget = request.target;
            LikesProgram::Http::HttpResponse response;
            response.statusCode = 204;
            response.reason = "No Content";
            return response;
        }

        // 记录真实 negotiated context，验证 Session 不丢失 ALPN/传输属性。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            const LikesProgram::Http::HttpNegotiatedProtocol& negotiated) override {
            lastNegotiated = negotiated;
            return Exchange(request, negotiated.version);
        }

        LikesProgram::Http::HttpVersion lastVersion = LikesProgram::Http::HttpVersion::Http1; // 最近协议
        std::string lastTarget; // 最近请求目标
        LikesProgram::Http::HttpNegotiatedProtocol lastNegotiated{}; // 最近连接上下文
    };

    class EchoHandler final : public LikesProgram::Http::HttpRequestHandler {
    public:
        // 将请求目标回显为响应正文，验证处理器完全独立于网络。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Handle(
            const LikesProgram::Http::HttpRequest& request) override {
            LikesProgram::Http::HttpResponse response;
            response.body.assign(request.target.begin(), request.target.end());
            return response;
        }
    };

    class FailingTransport final : public LikesProgram::Http::HttpTransport {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest&,
            LikesProgram::Http::HttpVersion) override {
            return LikesProgram::Status(LikesProgram::StatusCode::Unavailable,
                u"transport unavailable");
        }
    };

    class ReplayTransport final : public LikesProgram::Http::HttpTransport {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            LikesProgram::Http::HttpVersion version) override {
            versions.push_back(version);
            targets.push_back(request.target);
            if (versions.size() == 1) {
                return LikesProgram::Status(LikesProgram::StatusCode::Unavailable,
                    u"first protocol unavailable before response commit");
            }
            LikesProgram::Http::HttpResponse response;
            response.statusCode = 200;
            response.body = { 'r', 'e', 'p', 'l', 'a', 'y' };
            return response;
        }

        std::vector<LikesProgram::Http::HttpVersion> versions;
        std::vector<std::string> targets;
    };

    class ThrowingHandler final : public LikesProgram::Http::HttpRequestHandler {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Handle(
            const LikesProgram::Http::HttpRequest&) override {
            throw std::runtime_error("handler failure");
        }
    };

    class ThrowingTransport final : public LikesProgram::Http::HttpTransport {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest&,
            LikesProgram::Http::HttpVersion) override {
            throw std::runtime_error("transport failure");
        }
    };

    void TestHttp3VarIntAndFrameRoundTrip() {
        const std::vector<std::uint64_t> values = { 0, 63, 64, 16383, 16384,
            1073741823, 1073741824, (std::uint64_t{ 1 } << 62) - 1 }; // 四档边界
        for (const auto value : values) {
            const auto encoded = LikesProgram::Http::BuildHttp3VarInt(value);
            Require(encoded.IsOk(), "HTTP/3 varint should build at boundary");
            const auto decoded = LikesProgram::Http::ParseHttp3VarInt(
                encoded.Value().data(), encoded.Value().size());
            Require(decoded.IsOk() && decoded.Value().first == value,
                "HTTP/3 varint should round trip");
            Require(decoded.Value().second == encoded.Value().size(),
                "HTTP/3 varint width should round trip");
        }

        const std::vector<std::uint8_t> nonMinimal = { 0x40, 0x01 }; // 合法的非最短 QUIC varint
        const auto nonMinimalValue = LikesProgram::Http::ParseHttp3VarInt(
            nonMinimal.data(), nonMinimal.size());
        Require(nonMinimalValue.IsOk() && nonMinimalValue.Value().first == 1
            && nonMinimalValue.Value().second == 2,
            "HTTP/3 parser should accept a valid non-minimal QUIC varint");

        LikesProgram::Http::Http3Frame frame; // HTTP/3 DATA 帧
        frame.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
        frame.payload = { 'h', '3' };
        const auto built = LikesProgram::Http::BuildHttp3Frame(frame);
        Require(built.IsOk(), "HTTP/3 frame should build");
        const auto parsed = LikesProgram::Http::ParseHttp3Frame(built.Value());
        Require(parsed.IsOk() && parsed.Value().type == frame.type
            && parsed.Value().payload == frame.payload,
            "HTTP/3 frame should round trip");
        Require(std::string(LikesProgram::Http::Http3FrameTypeName(frame.type)) == "DATA",
            "HTTP/3 frame type name mismatch");
        Require(std::string(LikesProgram::Http::Http3FrameTypeName(0xFE)) == "UNKNOWN",
            "HTTP/3 unknown frame type should be preserved");
    }

    void TestHttp3ErrorsAndLargePayload() {
        Require(!LikesProgram::Http::ParseHttp3VarInt(nullptr, 0).IsOk(),
            "HTTP/3 parser should reject empty varint input");

        const std::vector<std::vector<std::uint8_t>> truncated = {
            { 0x40 }, { 0x80, 0, 0 }, { 0xC0, 0, 0, 0, 0, 0, 0 }
        }; // 2/4/8 字节变长整数均缺少尾字节
        for (const auto& bytes : truncated) {
            Require(!LikesProgram::Http::ParseHttp3VarInt(bytes.data(), bytes.size()).IsOk(),
                "HTTP/3 parser should reject truncated varint");
        }

        Require(!LikesProgram::Http::BuildHttp3VarInt(std::uint64_t{ 1 } << 62).IsOk(),
            "HTTP/3 builder should reject values above 62-bit range");

        const std::vector<std::uint8_t> missingLength = { 0x00 };
        Require(!LikesProgram::Http::ParseHttp3Frame(missingLength).IsOk(),
            "HTTP/3 parser should require a frame length");

        const std::vector<std::uint8_t> lengthMismatch = { 0x00, 0x02, 'x' };
        Require(!LikesProgram::Http::ParseHttp3Frame(lengthMismatch).IsOk(),
            "HTTP/3 parser should reject payload length mismatch");

        LikesProgram::Http::Http3Frame large;                 // 1 MiB 扩展帧
        large.type = 0xFE;
        large.payload.resize(1024 * 1024, 0x5A);
        const auto largeBytes = LikesProgram::Http::BuildHttp3Frame(large);
        Require(largeBytes.IsOk(), "HTTP/3 should build a 1 MiB frame");
        const auto largeParsed = LikesProgram::Http::ParseHttp3Frame(largeBytes.Value());
        Require(largeParsed.IsOk() && largeParsed.Value().payload == large.payload,
            "HTTP/3 should round trip a 1 MiB frame");
    }

    void TestHttp3StreamBodyDecoder() {
        LikesProgram::Http::Http3StreamBodyDecoder decoder(
            7, LikesProgram::Http::Http3StreamBodyLimits{ 3 });
        RequireStatus(decoder.AttachBodySink(nullptr), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 decoder should reject a null body sink");
        RequireStatus(decoder.Finish(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 stream should require FIN before Finish");

        LikesProgram::Http::Http3Frame first;
        first.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
        first.payload = { 'h', '3' };
        Require(decoder.Feed(7, first, false).IsOk(),
            "HTTP/3 stream should accept an intermediate DATA frame");

        LikesProgram::Http::Http3Frame end = first;
        end.payload = { '!' };
        Require(decoder.Feed(7, end, true).IsOk() && decoder.IsComplete(),
            "HTTP/3 stream should complete on QUIC FIN");
        Require(decoder.Finish().IsOk()
            && decoder.Body() == std::vector<std::uint8_t>{ 'h', '3', '!' },
            "HTTP/3 stream should finish with the concatenated body");
        RequireStatus(decoder.Feed(7, end, false), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream should reject DATA after FIN");

        decoder.Reset();
        LikesProgram::Http::Http3Frame empty;
        empty.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
        Require(decoder.Feed(7, empty, true).IsOk() && decoder.Body().empty(),
            "HTTP/3 stream should accept an empty DATA frame with FIN");

        decoder.Reset();
        RequireStatus(decoder.Feed(9, first, false), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream should reject a different QUIC stream id");
        decoder.Reset();
        LikesProgram::Http::Http3Frame wrongType = first;
        wrongType.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Headers);
        RequireStatus(decoder.Feed(7, wrongType, false), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream should reject non-DATA frames");
        decoder.Reset();
        LikesProgram::Http::Http3Frame overLimit = first;
        overLimit.payload = { '1', '2', '3', '4' };
        RequireStatus(decoder.Feed(7, overLimit, false), LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 stream should enforce its body limit");
        decoder.Reset();
        decoder.Cancel();
        Require(decoder.IsCancelled(), "HTTP/3 stream should expose cancellation");
        RequireStatus(decoder.Feed(7, first, false), LikesProgram::StatusCode::Cancelled,
            "HTTP/3 cancelled stream should reject further DATA");
        decoder.Reset();
        Require(decoder.Feed(7, empty, true).IsOk(),
            "HTTP/3 stream Reset should permit a new body");

        LikesProgram::Http::HttpBodySink sink({ 4, 1, 3 });
        LikesProgram::Http::Http3StreamBodyDecoder sinkDecoder(17);
        Require(sinkDecoder.AttachBodySink(&sink).IsOk() && sinkDecoder.HasBodySink(),
            "HTTP/3 stream should attach a bounded body sink before DATA");
        LikesProgram::Http::Http3Frame sinkFirst;
        sinkFirst.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
        sinkFirst.payload = { 'q', 'u' };
        Require(sinkDecoder.Feed(17, sinkFirst, false).IsOk()
            && sink.BufferedBytes() == 2 && sinkDecoder.Body().empty(),
            "HTTP/3 sink decoder should hand off DATA without retaining a vector body");
        auto sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'q', 'u' },
            "HTTP/3 body sink should expose handed-off bytes");

        LikesProgram::Http::Http3Frame sinkEnd = sinkFirst;
        sinkEnd.payload = { 'i', 'c' };
        Require(sinkDecoder.Feed(17, sinkEnd, true).IsOk()
            && sinkDecoder.IsComplete() && sink.IsClosed(),
            "HTTP/3 sink decoder should close the sink on stream FIN");
        sinkBytes = sink.Pull(4);
        Require(sinkBytes.IsOk() && sinkBytes.Value() == std::vector<std::uint8_t>{ 'i', 'c' },
            "HTTP/3 closed sink should retain the final DATA frame for draining");

        LikesProgram::Http::HttpBodySink blockedSink({ 2, 0, 2 });
        LikesProgram::Http::HttpBodyCancellation blockedBridge;
        LikesProgram::Http::Http3StreamBodyDecoder blockedDecoder(18);
        Require(blockedDecoder.AttachBodySink(&blockedSink).IsOk(),
            "HTTP/3 stream should attach a second body sink");
        Require(blockedSink.AttachCancellation(&blockedBridge).IsOk(),
            "HTTP/3 stream should attach a cancellation bridge");
        LikesProgram::Http::Http3Frame oversized = sinkFirst;
        oversized.payload = { '1', '2', '3' };
        RequireStatus(blockedDecoder.Feed(18, oversized, false),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 sink decoder should reject an oversized frame without partial writes");
        Require(blockedSink.BufferedBytes() == 0 && !blockedDecoder.IsCancelled(),
            "HTTP/3 sink backpressure should leave the decoder retryable");
        blockedSink.Reset();
        oversized.payload = { '1', '2' };
        Require(blockedDecoder.Feed(18, oversized, false).IsOk()
            && blockedSink.BufferedBytes() == 2,
            "HTTP/3 sink decoder should accept a frame after capacity is available");
        blockedDecoder.Reset();
        Require(blockedSink.State() == LikesProgram::Http::HttpBodySinkState::Open,
            "HTTP/3 decoder Reset should reset the attached sink");
        blockedDecoder.Cancel(LikesProgram::Http::HttpBodyCancelReason::PeerReset);
        Require(blockedSink.IsCancelled() && blockedDecoder.IsCancelled()
                && blockedBridge.Reason()
                    == LikesProgram::Http::HttpBodyCancelReason::PeerReset,
            "HTTP/3 decoder cancellation should propagate its reason to the sink");

        LikesProgram::Http::HttpBodySink limitedSink({ 3, 0, 3 });
        LikesProgram::Http::Http3StreamBodyDecoder limitedDecoder(
            19, LikesProgram::Http::Http3StreamBodyLimits{ 3 });
        Require(limitedDecoder.AttachBodySink(&limitedSink).IsOk(),
            "HTTP/3 limited decoder should attach a body sink");
        LikesProgram::Http::Http3Frame limitedFrame = sinkFirst;
        limitedFrame.payload = { 'a', 'b' };
        Require(limitedDecoder.Feed(19, limitedFrame, false).IsOk(),
            "HTTP/3 limited decoder should accept body within its cumulative limit");
        Require(limitedSink.Pull(3).IsOk(),
            "HTTP/3 limited decoder test should drain the first sink frame");
        limitedFrame.payload = { 'c', 'd' };
        RequireStatus(limitedDecoder.Feed(19, limitedFrame, false),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 sink decoder should retain its cumulative stream body limit");
    }

    void TestHttp3RequestStreamContract() {
        using namespace LikesProgram::Http;

        Http3RequestStream stream(0, Http3StreamBodyLimits{ 4 });
        const Http3Frame headers{ static_cast<std::uint64_t>(Http3FrameType::Headers),
            { 0x01, 0x02 } };
        const Http3Frame data{ static_cast<std::uint64_t>(Http3FrameType::Data),
            { 'o', 'k' } };
        const Http3Frame trailers{ static_cast<std::uint64_t>(Http3FrameType::Headers),
            { 0x03 } };
        const auto encodedHeaders = BuildHttp3Frame(headers);
        const auto encodedData = BuildHttp3Frame(data);
        const auto requestBytes = BuildHttp3RequestStreamBytes(
            headers.payload, { data.payload, {} });
        Require(encodedHeaders.IsOk() && encodedData.IsOk()
                && requestBytes.IsOk()
                && requestBytes.Value().size()
                    == encodedHeaders.Value().size() + encodedData.Value().size()
                && std::equal(encodedHeaders.Value().begin(), encodedHeaders.Value().end(),
                    requestBytes.Value().begin())
                && std::equal(encodedData.Value().begin(), encodedData.Value().end(),
                    requestBytes.Value().begin() + encodedHeaders.Value().size()),
            "HTTP/3 request stream encoder should concatenate opaque HEADERS and DATA frames");
        RequireStatus(BuildHttp3RequestStreamBytes({}, { data.payload }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream encoder should require an opaque HEADERS block");
        RequireStatus(BuildHttp3RequestStreamBytes(
                headers.payload, { { 'a', 'b', 'c', 'd', 'e' } },
                Http3RequestStreamEncodeLimits{ 64, 4, 8, 64 }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request stream encoder should enforce cumulative DATA limits");
        const auto encodedTrailers = BuildHttp3Frame(trailers);
        const auto responseBytes = BuildHttp3ResponseStreamBytes(
            headers.payload, { data.payload, {} }, trailers.payload);
        Require(encodedHeaders.IsOk() && encodedData.IsOk() && encodedTrailers.IsOk()
                && responseBytes.IsOk()
                && responseBytes.Value().size() == encodedHeaders.Value().size()
                    + encodedData.Value().size() + encodedTrailers.Value().size()
                && std::equal(encodedTrailers.Value().begin(), encodedTrailers.Value().end(),
                    responseBytes.Value().end() - encodedTrailers.Value().size()),
            "HTTP/3 response stream encoder should append optional trailing HEADERS after DATA");
        RequireStatus(BuildHttp3ResponseStreamBytes(
                headers.payload, {}, std::vector<std::uint8_t>{}),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 response stream encoder should reject an empty trailing HEADERS block");
        RequireStatus(BuildHttp3ResponseStreamBytes(
                headers.payload, { data.payload }, trailers.payload,
                Http3RequestStreamEncodeLimits{ 64, 64, 1, 64 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 response stream encoder should reserve frame budget for trailing HEADERS");
        const auto headersOnly = BuildHttp3ResponseStreamBytes(
            headers.payload, {}, std::nullopt,
            Http3RequestStreamEncodeLimits{ 64, 64, 0, 64 });
        Require(headersOnly.IsOk(),
            "HTTP/3 response stream encoder should allow HEADERS-only zero-DATA responses");
        RequireStatus(BuildHttp3ResponseStreamBytes(
                headers.payload, { data.payload }, std::nullopt,
                Http3RequestStreamEncodeLimits{ 64, 64, 0, 64 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 response stream encoder should reject DATA with zero frame budget");

        Require(stream.Feed(headers).IsOk()
                && stream.State() == Http3RequestStreamState::Open
                && stream.HeaderBlocks().size() == 1,
            "HTTP/3 request stream should require HEADERS as its first frame");
        Require(stream.Feed(data).IsOk()
                && stream.Body() == std::vector<std::uint8_t>{ 'o', 'k' },
            "HTTP/3 request stream should map DATA payloads to the body");
        Require(stream.Feed(trailers, true).IsOk()
                && stream.State() == Http3RequestStreamState::Complete
                && stream.TrailerBlocks().size() == 1,
            "HTTP/3 request stream should map a final HEADERS block to trailers");
        RequireStatus(stream.Feed(data), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should reject frames after FIN");

        stream.Reset();
        RequireStatus(stream.Feed(data), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should reject DATA before initial HEADERS");
        Require(stream.ErrorCode() == Http3ErrorCode::FrameUnexpected
                && stream.State() == Http3RequestStreamState::Failed,
            "HTTP/3 request stream should expose the mapped frame error");

        stream.Reset();
        Require(stream.Feed(headers).IsOk(),
            "HTTP/3 request stream should recover after Reset");
        const Http3Frame extension{ 0x2A, { 0xFF } };
        Require(stream.Feed(extension).IsOk()
                && stream.Snapshot().unknownFrameCount == 1,
            "HTTP/3 request stream should ignore unknown extension frames");
        const Http3Frame oversized{ static_cast<std::uint64_t>(Http3FrameType::Data),
            { 't', 'o', 'o', 'l', 'o', 'n', 'g' } };
        RequireStatus(stream.Feed(oversized), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should map body limit failure to H3 message error");
        Require(stream.ErrorCode() == Http3ErrorCode::MessageError,
            "HTTP/3 request stream should expose H3 message error mapping");

        Http3QpackDynamicTable qpackTable(128);
        auto buildStaticSection = [&](std::initializer_list<std::uint64_t> indexes) {
            auto prefix = BuildHttp3QpackFieldSectionPrefix({}, 128);
            Require(prefix.IsOk(),
                "HTTP/3 request stream QPACK fixture should encode its prefix");
            auto bytes = prefix.MoveValue();
            for (const auto index : indexes) {
                Http3QpackFieldLine field;
                field.type = Http3QpackFieldLineType::Indexed;
                field.staticTable = true;
                field.index = index;
                auto encoded = BuildHttp3QpackFieldLine(field);
                Require(encoded.IsOk(),
                    "HTTP/3 request stream QPACK fixture should encode a field");
                bytes.insert(bytes.end(), encoded.Value().begin(), encoded.Value().end());
            }
            return bytes;
        };
        Http3RequestStream decoded(3);
        const Http3Frame decodedHeaders{
            static_cast<std::uint64_t>(Http3FrameType::Headers),
            buildStaticSection({ 17, 23, 1 }) };
        Require(decoded.Feed(decodedHeaders).IsOk(),
            "HTTP/3 request stream should retain a decodable initial HEADERS block");
        auto requestHeaders = decoded.DecodeRequestHeaders(qpackTable);
        Require(requestHeaders.IsOk()
                && requestHeaders.Value().size() == 3
                && requestHeaders.Value()[0].name == ":method"
                && requestHeaders.Value()[0].value == "GET",
            "HTTP/3 request stream should decode and validate request HEADERS");
        Http3QpackSectionTracker sectionTracker;
        Require(decoded.TrackRequestHeaderSection(sectionTracker, qpackTable).IsOk()
                && sectionTracker.Snapshot().outstandingSections == 1,
            "HTTP/3 request stream should register initial HEADERS with QPACK tracker");
        Require(sectionTracker.AcknowledgeSection(3).IsOk()
                && sectionTracker.Snapshot().acknowledgedSections == 1,
            "HTTP/3 request stream should acknowledge tracked initial HEADERS");
        const Http3Frame decodedTrailers{
            static_cast<std::uint64_t>(Http3FrameType::Headers),
            buildStaticSection({ 2 }) };
        Require(decoded.Feed(decodedTrailers, true).IsOk(),
            "HTTP/3 request stream should retain a decodable trailer block");
        auto requestTrailers = decoded.DecodeTrailers(qpackTable);
        Require(requestTrailers.IsOk()
                && requestTrailers.Value().size() == 1
                && requestTrailers.Value()[0].name == "age"
                && requestTrailers.Value()[0].value == "0",
            "HTTP/3 request stream should decode and validate trailers");

        const std::size_t sharedHeaderLimit = std::max(
            decodedHeaders.payload.size(), decodedTrailers.payload.size());
        Http3QpackResourceBudget sharedBudget(
            Http3QpackLimits{ 128, 1, sharedHeaderLimit });
        Http3QpackDynamicTable sharedTable(128);
        Http3QpackSectionTracker sharedTracker;
        Require(sharedTable.AttachResourceBudget(&sharedBudget).IsOk()
                && sharedTable.SetCapacity(128).IsOk()
                && sharedTracker.AttachResourceBudget(&sharedBudget).IsOk()
                && sharedTracker.OpenSection(12, 1).IsOk(),
            "HTTP/3 request stream fixture should share independent QPACK budget dimensions");
        const auto requestBeforeBudgetDecode = decoded.Snapshot();
        const auto tableBeforeBudgetDecode = sharedTable.Snapshot();
        const auto trackerBeforeBudgetDecode = sharedTracker.Snapshot();
        const auto budgetBeforeBudgetDecode = sharedBudget.Snapshot();
        auto budgetRequestHeaders = decoded.DecodeRequestHeaders(
            sharedTable, sharedBudget);
        auto budgetRequestTrailers = decoded.DecodeTrailers(
            sharedTable, sharedBudget);
        const auto requestAfterBudgetDecode = decoded.Snapshot();
        const auto tableAfterBudgetDecode = sharedTable.Snapshot();
        const auto trackerAfterBudgetDecode = sharedTracker.Snapshot();
        const auto budgetAfterBudgetDecode = sharedBudget.Snapshot();
        Require(budgetRequestHeaders.IsOk()
                && budgetRequestTrailers.IsOk()
                && requestAfterBudgetDecode.state == requestBeforeBudgetDecode.state
                && requestAfterBudgetDecode.headerBlockCount
                    == requestBeforeBudgetDecode.headerBlockCount
                && requestAfterBudgetDecode.trailerBlockCount
                    == requestBeforeBudgetDecode.trailerBlockCount
                && requestAfterBudgetDecode.bodyBytes
                    == requestBeforeBudgetDecode.bodyBytes
                && tableAfterBudgetDecode.capacity == tableBeforeBudgetDecode.capacity
                && tableAfterBudgetDecode.bytes == tableBeforeBudgetDecode.bytes
                && tableAfterBudgetDecode.insertCount
                    == tableBeforeBudgetDecode.insertCount
                && trackerAfterBudgetDecode.outstandingSections
                    == trackerBeforeBudgetDecode.outstandingSections
                && trackerAfterBudgetDecode.blockedStreams
                    == trackerBeforeBudgetDecode.blockedStreams
                && budgetAfterBudgetDecode.dynamicTableCapacity
                    == budgetBeforeBudgetDecode.dynamicTableCapacity
                && budgetAfterBudgetDecode.dynamicTableBytes
                    == budgetBeforeBudgetDecode.dynamicTableBytes
                && budgetAfterBudgetDecode.blockedStreams
                    == budgetBeforeBudgetDecode.blockedStreams,
            "HTTP/3 request stream budget-aware decode should preserve every resource snapshot");

        Http3QpackResourceBudget strictHeaderBudget(
            Http3QpackLimits{ 128, 1, 0 });
        RequireStatus(decoded.DecodeRequestHeaders(qpackTable, strictHeaderBudget),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request stream should enforce a stricter QPACK header budget");
        RequireStatus(decoded.DecodeTrailers(qpackTable, strictHeaderBudget),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request stream should enforce a stricter QPACK trailer budget");
        const auto requestAfterBudgetRejection = decoded.Snapshot();
        Require(requestAfterBudgetRejection.state == requestBeforeBudgetDecode.state
                && requestAfterBudgetRejection.headerBlockCount
                    == requestBeforeBudgetDecode.headerBlockCount
                && requestAfterBudgetRejection.trailerBlockCount
                    == requestBeforeBudgetDecode.trailerBlockCount,
            "HTTP/3 request stream budget rejection should preserve request state");

        Http3QpackResourceBudget movedFromHeaderBudget(
            Http3QpackLimits{ 128, 1, sharedHeaderLimit });
        Http3QpackResourceBudget liveHeaderBudget(
            std::move(movedFromHeaderBudget));
        Require(liveHeaderBudget.ValidateHeaderBlock(sharedHeaderLimit).IsOk(),
            "HTTP/3 request stream fixture should preserve the moved-to budget");
        RequireStatus(decoded.DecodeRequestHeaders(qpackTable, movedFromHeaderBudget),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 request stream should reject a moved-from QPACK budget");
        Require(decoded.TrackTrailerSection(sectionTracker, qpackTable).IsOk()
                && sectionTracker.Snapshot().outstandingSections == 1,
            "HTTP/3 request stream should register trailer HEADERS with QPACK tracker");
        Require(sectionTracker.AcknowledgeSection(3).IsOk()
                && sectionTracker.Snapshot().acknowledgedSections == 2,
            "HTTP/3 request stream should acknowledge tracked trailer HEADERS");

        Http3RequestStream emptyDecoded(4);
        RequireStatus(emptyDecoded.DecodeRequestHeaders(qpackTable),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream should require HEADERS before QPACK decoding");
        RequireStatus(emptyDecoded.TrackRequestHeaderSection(sectionTracker, qpackTable),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream should require HEADERS before QPACK tracking");

        Http3RequestStream bounded(2, Http3StreamBodyLimits{ 4, 1, 1, 1 });
        RequireStatus(bounded.Feed(headers), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should enforce a header block limit");
        bounded.Reset();
        const Http3Frame shortHeaders{ static_cast<std::uint64_t>(Http3FrameType::Headers),
            { 0x01 } };
        Require(bounded.Feed(shortHeaders).IsOk()
                && bounded.Feed(extension).IsOk(),
            "HTTP/3 request stream should accept bounded headers and one extension");
        RequireStatus(bounded.Feed(extension), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should enforce an unknown frame limit");
        bounded.Reset();
        Require(bounded.Feed(shortHeaders).IsOk(),
            "HTTP/3 request stream should reset bounded limit state");
        RequireStatus(bounded.Feed(headers, true), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should enforce a trailer block limit");

        const auto wireHeaders = BuildHttp3Frame(shortHeaders);
        const auto wireData = BuildHttp3Frame(data);
        Require(wireHeaders.IsOk() && wireData.IsOk(),
            "HTTP/3 request stream wire fixture should build complete frames");
        Http3RequestStreamWireDecoder wire(5);
        std::vector<std::uint8_t> wireBytes = wireHeaders.Value();
        wireBytes.insert(wireBytes.end(), wireData.Value().begin(), wireData.Value().end());
        for (const auto byte : wireBytes) {
            Require(wire.Feed(&byte, 1).IsOk(),
                "HTTP/3 request stream wire decoder should hold partial varints and frames");
        }
        Require(wire.PendingBytes() == 0
                && wire.Finish().IsOk()
                && wire.IsComplete()
                && wire.Snapshot().streamId == 5
                && wire.Body() == std::vector<std::uint8_t>{ 'o', 'k' },
            "HTTP/3 request stream wire decoder should map independent FIN to completion");
        RequireStatus(wire.Feed(std::vector<std::uint8_t>{}, true),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream wire decoder should reject data after FIN");

        Http3RequestStreamWireDecoder truncated(6);
        RequireStatus(truncated.Feed(wireHeaders.Value().data(), 1, true),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream wire decoder should reject FIN with an incomplete frame");
        Require(truncated.LastError().Code() == LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream wire decoder should expose local wire failure status");
        const auto truncatedActions = truncated.FailureActions();
        Require(truncatedActions.IsOk()
                && truncatedActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::MessageError)
                && truncatedActions.Value().resetStream
                && truncatedActions.Value().stopSending,
            "HTTP/3 request stream wire decoder should map local wire failure to H3 actions");
        Http3RequestStreamWireDecoder boundedWire(7, {}, { 4, 2 });
        const Http3Frame oversizedData{ static_cast<std::uint64_t>(Http3FrameType::Data),
            { 'o', 'k', '!' } };
        const auto oversizedWire = BuildHttp3Frame(oversizedData);
        Require(oversizedWire.IsOk(),
            "HTTP/3 request stream wire decoder should build its limit fixture");
        RequireStatus(boundedWire.Feed(oversizedWire.Value()),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request stream wire decoder should enforce frame payload limits");
        Http3RequestStreamWireDecoder emptyFin(8);
        RequireStatus(emptyFin.Finish(), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream wire decoder should reject FIN before initial HEADERS");
        Require(emptyFin.Snapshot().errorCode == Http3ErrorCode::FrameUnexpected,
            "HTTP/3 request stream wire decoder should expose FIN ordering errors");
        RequireStatus(wire.FailureActions(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream wire decoder should require a failure before mapping actions");

        const auto errorActions = MapHttp3RequestStreamError(stream.ErrorCode());
        Require(errorActions.IsOk()
                && errorActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::MessageError)
                && errorActions.Value().resetStream
                && errorActions.Value().stopSending,
            "HTTP/3 request stream should expose non-owning RESET/STOP action mapping");
        RequireStatus(MapHttp3RequestStreamError(Http3ErrorCode::NoError),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should reject a no-error action mapping");
    }

    void TestHttp3RequestStreamBodySinkLifecycle() {
        using namespace LikesProgram::Http;

        const Http3Frame headers{
            static_cast<std::uint64_t>(Http3FrameType::Headers), { 0x01 } };
        const Http3Frame data{
            static_cast<std::uint64_t>(Http3FrameType::Data), { 'o', 'k' } };
        const Http3Frame trailer{
            static_cast<std::uint64_t>(Http3FrameType::Headers), { 0x02 } };
        const auto wireHeaders = BuildHttp3Frame(headers);
        const auto wireData = BuildHttp3Frame(data);
        Require(wireHeaders.IsOk() && wireData.IsOk(),
            "HTTP/3 request body-sink fixture should build wire frames");

        HttpBodySink directSink({ 4, 1, 4 });
        HttpBodySink otherSink({ 4, 1, 4 });
        Http3RequestStream direct(4);
        RequireStatus(direct.AttachBodySink(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request stream should reject a null body sink");
        Require(direct.AttachBodySink(&directSink).IsOk()
                && direct.AttachBodySink(&directSink).IsOk()
                && direct.HasBodySink(),
            "HTTP/3 request stream should idempotently attach one body sink");
        RequireStatus(direct.AttachBodySink(&otherSink),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream should reject a conflicting body sink");
        Require(direct.Feed(headers).IsOk()
                && direct.Feed(data).IsOk()
                && direct.Body().empty()
                && direct.Snapshot().bodyBytes == data.payload.size()
                && directSink.BufferedBytes() == data.payload.size(),
            "HTTP/3 request stream should hand DATA to the sink without a duplicate body");
        RequireStatus(direct.AttachBodySink(&otherSink),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request stream should reject late sink attachment");
        Require(direct.Feed(trailer, true).IsOk()
                && directSink.IsClosed(),
            "HTTP/3 request stream FIN should close the body sink for draining");
        auto directBody = directSink.Pull(4);
        Require(directBody.IsOk() && directBody.Value() == data.payload,
            "HTTP/3 request stream closed sink should retain DATA for draining");
        direct.Reset();
        Require(direct.HasBodySink()
                && directSink.State() == HttpBodySinkState::Open
                && direct.Snapshot().bodyBytes == 0,
            "HTTP/3 request stream Reset should reopen the attached sink");

        HttpBodyBudget wireBudget({ 4, 4 });
        HttpBodySink wireSink({ 2, 0, 2 });
        Http3RequestStreamWireDecoder wire(8);
        const std::vector<std::uint8_t> filler{ 'f' };
        Require(wireSink.AttachBudget(&wireBudget, 8).IsOk()
                && wire.AttachBodySink(&wireSink).IsOk()
                && wire.HasBodySink()
                && wire.Feed(wireHeaders.Value()).IsOk()
                && wireSink.Push(filler.data(), filler.size()).Value() == 1,
            "HTTP/3 request wire fixture should leave one sink byte available");
        RequireStatus(wire.Feed(wireData.Value(), true),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request wire decoder should retain a complete frame on backpressure");
        Require(wire.HasPendingBodyRetry()
                && wire.PendingBytes() == wireData.Value().size()
                && wire.Snapshot().bodyBytes == 0
                && wire.Body().empty()
                && wire.LastError().IsOk()
                && wireBudget.ReservedBytes(8) == 1,
            "HTTP/3 request wire backpressure should preserve frame, FIN, budget and error state");
        RequireStatus(wire.FailureActions(),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request wire backpressure should not become an H3 failure");
        RequireStatus(wire.Feed(nullptr, 0, false),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request wire decoder should require its explicit retry entry");
        RequireStatus(wire.RetryPendingBody(),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 request wire retry should remain pending while capacity is unavailable");
        auto fillerBody = wireSink.Pull(1);
        Require(fillerBody.IsOk() && fillerBody.Value() == filler
                && wireBudget.ReservedBytes() == 0
                && wire.RetryPendingBody().IsOk()
                && !wire.HasPendingBodyRetry()
                && wire.PendingBytes() == 0
                && wire.IsComplete()
                && wire.Snapshot().bodyBytes == data.payload.size()
                && wire.Body().empty()
                && wireSink.IsClosed()
                && wireBudget.ReservedBytes(8) == data.payload.size(),
            "HTTP/3 request wire retry should commit the retained frame and FIN once");
        auto wireBody = wireSink.Pull(4);
        Require(wireBody.IsOk()
                && wireBody.Value() == data.payload
                && wireBudget.ReservedBytes() == 0,
            "HTTP/3 request wire sink should release shared budget when drained");
        RequireStatus(wire.RetryPendingBody(),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request wire decoder should consume a body retry once");
        wire.Reset();
        Require(wire.HasBodySink()
                && wireSink.State() == HttpBodySinkState::Open
                && !wire.HasPendingBodyRetry(),
            "HTTP/3 request wire Reset should retain and reopen its sink");

        HttpBodySink protocolSink({ 4, 1, 4 });
        HttpBodyCancellation protocolCancellation;
        Http3RequestStreamWireDecoder limitedWire(
            12, Http3StreamBodyLimits{ 2 });
        const Http3Frame oneByteData{
            static_cast<std::uint64_t>(Http3FrameType::Data), { 'x' } };
        const auto oneByteWire = BuildHttp3Frame(oneByteData);
        Require(oneByteWire.IsOk()
                && protocolSink.AttachCancellation(&protocolCancellation).IsOk()
                && limitedWire.AttachBodySink(&protocolSink).IsOk()
                && limitedWire.Feed(wireHeaders.Value()).IsOk()
                && limitedWire.Feed(wireData.Value()).IsOk(),
            "HTTP/3 request wire cumulative-limit fixture should accept its first DATA");
        Require(protocolSink.Pull(4).IsOk(),
            "HTTP/3 request wire cumulative-limit fixture should drain its first DATA");
        RequireStatus(limitedWire.Feed(oneByteWire.Value()),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 request wire decoder should enforce cumulative body limits with a sink");
        Require(protocolSink.IsCancelled()
                && protocolCancellation.Reason()
                    == HttpBodyCancelReason::ProtocolError
                && !limitedWire.HasPendingBodyRetry(),
            "HTTP/3 request wire protocol failure should cancel rather than backpressure the sink");

        HttpBodyBudget bridgeBudget({ 4, 4 });
        HttpBodySink bridgeSink({ 2, 0, 2 });
        HttpBodyCancellation bridgeCancellation;
        Http3QuicRequestStreamBridge bridge(16);
        Require(bridgeSink.AttachBudget(&bridgeBudget, 16).IsOk()
                && bridgeSink.AttachCancellation(&bridgeCancellation).IsOk()
                && bridge.AttachBodySink(&bridgeSink).IsOk()
                && bridge.HasBodySink()
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    16, 0, wireHeaders.Value() }).IsOk()
                && bridgeSink.Push(filler.data(), filler.size()).Value() == 1,
            "HTTP/3 QUIC request bridge fixture should attach a budgeted sink");
        RequireStatus(bridge.Feed({ Http3QuicEventKind::StreamFin,
                16, 0, wireData.Value() }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC request bridge should expose retryable body backpressure");
        const auto blocked = bridge.Snapshot();
        Require(blocked.hasBodySink
                && blocked.bodyBlocked
                && blocked.pendingWireBytes == wireData.Value().size()
                && blocked.pendingBodyBytes == data.payload.size()
                && blocked.bodyBufferedBytes == 1
                && blocked.bodyWritableBytes == 1
                && blocked.bodyCapacityBytes == 2
                && !blocked.bodyRetryReady
                && bridge.LastError().IsOk(),
            "HTTP/3 QUIC request bridge should expose its pending body retry");
        RequireStatus(bridge.Feed({ Http3QuicEventKind::StreamData, 16 }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC request bridge should reject new bytes while body-blocked");
        RequireStatus(bridge.RetryPendingBody(),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC request bridge should remain blocked without capacity");
        const auto repeatedlyBlocked = bridge.Snapshot();
        Require(repeatedlyBlocked.pendingBodyBytes == data.payload.size()
                && repeatedlyBlocked.pendingWireBytes == wireData.Value().size()
                && repeatedlyBlocked.bodyBufferedBytes == 1
                && repeatedlyBlocked.bodyWritableBytes == 1
                && !repeatedlyBlocked.bodyRetryReady,
            "HTTP/3 QUIC request bridge should preserve blocked capacity coordinates");
        Require(bridgeSink.Pull(1).IsOk(),
            "HTTP/3 QUIC request bridge fixture should drain its filler");
        const auto retryReady = bridge.Snapshot();
        Require(retryReady.bodyBufferedBytes == 0
                && retryReady.bodyWritableBytes == 2
                && retryReady.bodyRetryReady,
            "HTTP/3 QUIC request bridge should observe enough retry capacity");
        bridgeSink.Pause();
        Require(bridge.Snapshot().bodyWritableBytes == 0
                && !bridge.Snapshot().bodyRetryReady,
            "HTTP/3 QUIC request bridge should not retry a paused sink");
        bridgeSink.Resume();
        Require(bridge.Snapshot().bodyRetryReady
                && bridge.RetryPendingBody().IsOk(),
            "HTTP/3 QUIC request bridge should retry after sink resume");
        const auto retried = bridge.Snapshot();
        Require(!retried.bodyBlocked
                && retried.pendingBodyBytes == 0
                && retried.bodyBufferedBytes == data.payload.size()
                && retried.bodyWritableBytes == 0
                && retried.bodyCapacityBytes == 2
                && !retried.bodyRetryReady
                && retried.stream.bodyBytes == data.payload.size()
                && bridgeSink.IsClosed(),
            "HTTP/3 QUIC request bridge retry should preserve FIN and close the sink");
        Require(bridgeSink.Pull(4).IsOk()
                && bridgeBudget.ReservedBytes() == 0,
            "HTTP/3 QUIC request bridge sink should drain and release budget");
        bridge.Reset();
        Require(bridgeSink.State() == HttpBodySinkState::Open
                && bridge.Snapshot().pendingBodyBytes == 0
                && bridge.Snapshot().bodyBufferedBytes == 0
                && bridge.Snapshot().bodyWritableBytes == 2
                && bridge.Snapshot().bodyCapacityBytes == 2
                && !bridge.Snapshot().bodyRetryReady
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    16, 0, wireHeaders.Value() }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamReset,
                    16, 0x10C }).IsOk()
                && bridgeSink.IsCancelled()
                && bridgeCancellation.Reason() == HttpBodyCancelReason::PeerReset,
            "HTTP/3 QUIC request bridge peer reset should cancel its attached sink");

        Http3RequestStreamWireDecoder movedWire(20);
        Http3RequestStreamWireDecoder liveWire(std::move(movedWire));
        RequireStatus(movedWire.AttachBodySink(&otherSink),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from request wire decoder should reject sink attachment");
        RequireStatus(movedWire.RetryPendingBody(),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from request wire decoder should reject body retry");
        Require(!movedWire.HasBodySink()
                && movedWire.PendingBodyBytes() == 0
                && liveWire.LastError().IsOk(),
            "HTTP/3 moved request wire decoder should preserve the live object");

        HttpBodySink movingBridgeSink({ 2, 0, 2 });
        Http3QuicRequestStreamBridge movingBridge(24);
        Require(movingBridge.AttachBodySink(&movingBridgeSink).IsOk()
                && movingBridge.Feed({ Http3QuicEventKind::StreamData,
                    24, 0, wireHeaders.Value() }).IsOk()
                && movingBridgeSink.Push(filler.data(), filler.size()).IsOk(),
            "HTTP/3 moving request bridge fixture should attach a sink");
        RequireStatus(movingBridge.Feed({ Http3QuicEventKind::StreamData,
                24, 0, wireData.Value() }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 moving request bridge fixture should retain blocked DATA");
        Http3QuicRequestStreamBridge liveBridge(std::move(movingBridge));
        Require(!movingBridge.Snapshot().hasBodySink
                && movingBridge.Snapshot().pendingBodyBytes == 0
                && liveBridge.Snapshot().pendingBodyBytes == data.payload.size()
                && liveBridge.Snapshot().bodyBufferedBytes == 1
                && !liveBridge.Snapshot().bodyRetryReady
                && movingBridgeSink.Pull(1).IsOk()
                && liveBridge.Snapshot().bodyRetryReady
                && liveBridge.RetryPendingBody().IsOk()
                && liveBridge.Snapshot().pendingBodyBytes == 0,
            "HTTP/3 request bridge move should transfer receive-capacity state");
    }

    void TestHttp3QpackResourceBudget() {
        using namespace LikesProgram::Http;

        Http3QpackResourceBudget budget(Http3QpackLimits{ 8, 1, 4 });
        Require(budget.SetDynamicTableCapacity(8).IsOk()
                && budget.ReserveDynamicTable(6).IsOk(),
            "HTTP/3 QPACK budget should reserve within negotiated capacity");
        RequireStatus(budget.ReserveDynamicTable(3),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK budget should reject dynamic table overflow");
        RequireStatus(budget.SetDynamicTableCapacity(4),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK budget should not shrink below current usage");
        Require(budget.ReleaseDynamicTable(2).IsOk()
                && budget.SetDynamicTableCapacity(4).IsOk(),
            "HTTP/3 QPACK budget should permit a safe capacity reduction");
        RequireStatus(budget.ValidateHeaderBlock(5),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK budget should enforce the header block limit");
        Require(budget.OpenBlockedStream(0).IsOk(),
            "HTTP/3 QPACK budget should track a blocked request stream");
        RequireStatus(budget.OpenBlockedStream(0),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK budget should reject a duplicate blocked stream");
        RequireStatus(budget.OpenBlockedStream(4),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK budget should enforce the blocked stream limit");
        Require(budget.CloseBlockedStream(0).IsOk()
                && budget.OpenBlockedStream(4).IsOk()
                && budget.Snapshot().dynamicTableBytes == 4
                && budget.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK budget should expose current resource accounting");
        budget.Reset();
        Require(budget.Snapshot().dynamicTableCapacity == 0
                && budget.Snapshot().dynamicTableBytes == 0
                && budget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK budget reset should clear usage and retain limits");
    }

    void TestHttp3QpackPrefixedInteger() {
        using namespace LikesProgram::Http;

        const auto rfcExample = BuildHttp3QpackPrefixedInteger(1337, 5);
        Require(rfcExample.IsOk()
                && rfcExample.Value()
                    == std::vector<std::uint8_t>({ 0x1F, 0x9A, 0x0A }),
            "HTTP/3 QPACK integer should match the RFC prefixed integer example");
        const auto parsedExample = ParseHttp3QpackPrefixedInteger(
            rfcExample.Value().data(), rfcExample.Value().size(), 5);
        Require(parsedExample.IsOk()
                && parsedExample.Value().first == 1337
                && parsedExample.Value().second == 3,
            "HTTP/3 QPACK integer should parse the RFC prefixed integer example");

        const auto flagged = BuildHttp3QpackPrefixedInteger(31, 5, 0x20);
        Require(flagged.IsOk()
                && flagged.Value() == std::vector<std::uint8_t>({ 0x3F, 0x00 }),
            "HTTP/3 QPACK integer should preserve instruction flags at the prefix limit");
        const auto parsedFlagged = ParseHttp3QpackPrefixedInteger(
            flagged.Value().data(), flagged.Value().size(), 5);
        Require(parsedFlagged.IsOk() && parsedFlagged.Value().first == 31,
            "HTTP/3 QPACK integer parser should ignore instruction flag bits");

        constexpr std::uint64_t maxQpackInteger =
            (std::uint64_t{ 1 } << 62) - 1;
        const auto maximum = BuildHttp3QpackPrefixedInteger(maxQpackInteger, 1, 0x80);
        Require(maximum.IsOk(),
            "HTTP/3 QPACK integer should encode the required 62-bit maximum");
        const auto parsedMaximum = ParseHttp3QpackPrefixedInteger(
            maximum.Value().data(), maximum.Value().size(), 1);
        Require(parsedMaximum.IsOk()
                && parsedMaximum.Value().first == maxQpackInteger
                && parsedMaximum.Value().second == maximum.Value().size(),
            "HTTP/3 QPACK integer should round-trip the required 62-bit maximum");

        const std::uint8_t truncated[] = { 0x1F, 0x80 };
        RequireStatus(ParseHttp3QpackPrefixedInteger(truncated, 2, 5),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK integer should reject truncated continuation bytes");
        const std::uint8_t overflow[] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0x7F
        };
        RequireStatus(ParseHttp3QpackPrefixedInteger(overflow,
                sizeof(overflow), 8),
            LikesProgram::StatusCode::OutOfRange,
            "HTTP/3 QPACK integer should reject values beyond 62 bits");
        RequireStatus(BuildHttp3QpackPrefixedInteger(maxQpackInteger + 1, 5),
            LikesProgram::StatusCode::OutOfRange,
            "HTTP/3 QPACK integer should not encode values beyond 62 bits");
        RequireStatus(BuildHttp3QpackPrefixedInteger(1, 5, 0x01),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK integer should reject overlapping instruction flags");
        RequireStatus(ParseHttp3QpackPrefixedInteger(truncated, 2, 0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK integer should reject an empty prefix width");
    }

    void TestHttp3QpackStringLiteral() {
        using namespace LikesProgram::Http;

        Http3QpackStringLiteral raw;
        raw.bytes = { 'a', 'b', 'c' };
        const auto encoded = BuildHttp3QpackStringLiteral(raw, 8);
        Require(encoded.IsOk()
                && encoded.Value() == std::vector<std::uint8_t>({ 3, 'a', 'b', 'c' }),
            "HTTP/3 QPACK raw string literal should encode its length and bytes");
        const auto parsed = ParseHttp3QpackStringLiteral(
            encoded.Value().data(), encoded.Value().size(), 8);
        Require(parsed.IsOk()
                && !parsed.Value().first.huffmanEncoded
                && parsed.Value().first.bytes == raw.bytes
                && parsed.Value().second == encoded.Value().size(),
            "HTTP/3 QPACK raw string literal should round-trip");

        Http3QpackStringLiteral huffman;
        huffman.huffmanEncoded = true;
        huffman.bytes = { 0xF1, 0x00 };
        const auto encodedHuffman = BuildHttp3QpackStringLiteral(huffman, 6, 0xC0);
        Require(encodedHuffman.IsOk()
                && encodedHuffman.Value()
                    == std::vector<std::uint8_t>({ 0xE2, 0xF1, 0x00 }),
            "HTTP/3 QPACK string literal should preserve Huffman and instruction flags");
        const auto parsedHuffman = ParseHttp3QpackStringLiteral(
            encodedHuffman.Value().data(), encodedHuffman.Value().size(), 6);
        Require(parsedHuffman.IsOk()
                && parsedHuffman.Value().first.huffmanEncoded
                && parsedHuffman.Value().first.bytes == huffman.bytes,
            "HTTP/3 QPACK string literal should expose opaque Huffman bytes");

        Http3QpackStringLiteral shortName;
        shortName.bytes = { 'x' };
        const auto encodedShort = BuildHttp3QpackStringLiteral(shortName, 6, 0x40);
        Require(encodedShort.IsOk()
                && encodedShort.Value() == std::vector<std::uint8_t>({ 0x41, 'x' }),
            "HTTP/3 QPACK string literal should support a six-bit length prefix");
        const std::uint8_t truncated[] = { 0x02, 'x' };
        RequireStatus(ParseHttp3QpackStringLiteral(truncated, 1, 8),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK string literal should reject truncated payloads");
        RequireStatus(BuildHttp3QpackStringLiteral(raw, 1),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK string literal should reject a one-bit prefix");
        RequireStatus(BuildHttp3QpackStringLiteral(raw, 9),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK string literal should reject a nine-bit prefix");
        RequireStatus(BuildHttp3QpackStringLiteral(raw, 6, 0x01),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK string literal should reject flags in the length prefix");
    }

    void TestHttp3QpackHuffman() {
        using namespace LikesProgram::Http;

        const std::uint8_t example[] = {
            0xF1, 0xE3, 0xC2, 0xE5, 0xF2, 0x3A,
            0x6B, 0xA0, 0xAB, 0x90, 0xF4, 0xFF
        };
        const auto decoded = DecodeHttp3QpackHuffman(example, sizeof(example));
        Require(decoded.IsOk()
                && decoded.Value() == std::vector<std::uint8_t>({
                    'w', 'w', 'w', '.', 'e', 'x', 'a', 'm',
                    'p', 'l', 'e', '.', 'c', 'o', 'm' }),
            "HTTP/3 QPACK Huffman should decode the RFC www.example.com example");

        const std::uint8_t paddedZero[] = { 0x07 };
        const auto decodedZero = DecodeHttp3QpackHuffman(
            paddedZero, sizeof(paddedZero));
        Require(decodedZero.IsOk()
                && decodedZero.Value() == std::vector<std::uint8_t>({ '0' }),
            "HTTP/3 QPACK Huffman should accept an EOS-prefix padding suffix");

        const auto empty = DecodeHttp3QpackHuffman(nullptr, 0);
        Require(empty.IsOk() && empty.Value().empty(),
            "HTTP/3 QPACK Huffman should accept an empty payload");
        RequireStatus(DecodeHttp3QpackHuffman(example, sizeof(example), 14),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK Huffman should enforce the decoded-byte limit");
        RequireStatus(DecodeHttp3QpackHuffman(nullptr, 1),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK Huffman should reject null non-empty input");

        const std::uint8_t eos[] = { 0xFF, 0xFF, 0xFF, 0xFF };
        RequireStatus(DecodeHttp3QpackHuffman(eos, sizeof(eos)),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK Huffman should reject EOS inside a payload");
        const std::uint8_t badPadding[] = { 0x00 };
        RequireStatus(DecodeHttp3QpackHuffman(badPadding, sizeof(badPadding)),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK Huffman should reject non-EOS padding bits");
        const std::uint8_t longPadding[] = { 0xFF };
        RequireStatus(DecodeHttp3QpackHuffman(longPadding, sizeof(longPadding)),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK Huffman should reject padding longer than seven bits");
    }

    void TestHttp3QpackInstructions() {
        using namespace LikesProgram::Http;

        Http3QpackEncoderInstruction capacity;
        capacity.value = 31;
        auto encodedCapacity = BuildHttp3QpackEncoderInstruction(capacity);
        Require(encodedCapacity.IsOk()
                && encodedCapacity.Value() == std::vector<std::uint8_t>({ 0x3F, 0x00 }),
            "HTTP/3 QPACK encoder should build Set Dynamic Table Capacity");
        auto parsedCapacity = ParseHttp3QpackEncoderInstruction(
            encodedCapacity.Value().data(), encodedCapacity.Value().size());
        Require(parsedCapacity.IsOk()
                && parsedCapacity.Value().first.type
                    == Http3QpackEncoderInstructionType::SetDynamicTableCapacity
                && parsedCapacity.Value().first.value == 31,
            "HTTP/3 QPACK encoder should parse Set Dynamic Table Capacity");

        Http3QpackEncoderInstruction nameReference;
        nameReference.type = Http3QpackEncoderInstructionType::InsertWithNameReference;
        nameReference.value = 5;
        nameReference.staticTable = true;
        nameReference.fieldValue.bytes = { 'o', 'k' };
        auto encodedNameReference = BuildHttp3QpackEncoderInstruction(nameReference);
        Require(encodedNameReference.IsOk()
                && encodedNameReference.Value()
                    == std::vector<std::uint8_t>({ 0xC5, 0x02, 'o', 'k' }),
            "HTTP/3 QPACK encoder should build an indexed-name insertion");
        auto parsedNameReference = ParseHttp3QpackEncoderInstruction(
            encodedNameReference.Value().data(), encodedNameReference.Value().size());
        Require(parsedNameReference.IsOk()
                && parsedNameReference.Value().first.staticTable
                && parsedNameReference.Value().first.value == 5
                && parsedNameReference.Value().first.fieldValue.bytes
                    == nameReference.fieldValue.bytes,
            "HTTP/3 QPACK encoder should parse an indexed-name insertion");

        Http3QpackEncoderInstruction literalName;
        literalName.type = Http3QpackEncoderInstructionType::InsertWithLiteralName;
        literalName.name.bytes = { 'x' };
        literalName.fieldValue.bytes = { 'y' };
        auto encodedLiteralName = BuildHttp3QpackEncoderInstruction(literalName);
        Require(encodedLiteralName.IsOk()
                && encodedLiteralName.Value()
                    == std::vector<std::uint8_t>({ 0x41, 'x', 0x01, 'y' }),
            "HTTP/3 QPACK encoder should build a literal-name insertion");
        auto parsedLiteralName = ParseHttp3QpackEncoderInstruction(
            encodedLiteralName.Value().data(), encodedLiteralName.Value().size());
        Require(parsedLiteralName.IsOk()
                && parsedLiteralName.Value().first.name.bytes
                    == literalName.name.bytes
                && parsedLiteralName.Value().first.fieldValue.bytes
                    == literalName.fieldValue.bytes,
            "HTTP/3 QPACK encoder should parse a literal-name insertion");

        Http3QpackEncoderInstruction duplicate;
        duplicate.type = Http3QpackEncoderInstructionType::Duplicate;
        duplicate.value = 2;
        auto encodedDuplicate = BuildHttp3QpackEncoderInstruction(duplicate);
        Require(encodedDuplicate.IsOk()
                && encodedDuplicate.Value() == std::vector<std::uint8_t>({ 0x02 }),
            "HTTP/3 QPACK encoder should build Duplicate");

        Http3QpackDecoderInstruction acknowledgment;
        acknowledgment.value = 7;
        auto encodedAcknowledgment = BuildHttp3QpackDecoderInstruction(acknowledgment);
        Require(encodedAcknowledgment.IsOk()
                && encodedAcknowledgment.Value() == std::vector<std::uint8_t>({ 0x87 }),
            "HTTP/3 QPACK decoder should build Section Acknowledgment");
        auto parsedAcknowledgment = ParseHttp3QpackDecoderInstruction(
            encodedAcknowledgment.Value().data(), encodedAcknowledgment.Value().size());
        Require(parsedAcknowledgment.IsOk()
                && parsedAcknowledgment.Value().first.value == 7,
            "HTTP/3 QPACK decoder should parse Section Acknowledgment");

        Http3QpackDecoderInstruction cancellation;
        cancellation.type = Http3QpackDecoderInstructionType::StreamCancellation;
        cancellation.value = 3;
        auto encodedCancellation = BuildHttp3QpackDecoderInstruction(cancellation);
        Require(encodedCancellation.IsOk()
                && encodedCancellation.Value() == std::vector<std::uint8_t>({ 0x43 }),
            "HTTP/3 QPACK decoder should build Stream Cancellation");

        Http3QpackDecoderInstruction increment;
        increment.type = Http3QpackDecoderInstructionType::InsertCountIncrement;
        increment.value = 2;
        auto encodedIncrement = BuildHttp3QpackDecoderInstruction(increment);
        Require(encodedIncrement.IsOk()
                && encodedIncrement.Value() == std::vector<std::uint8_t>({ 0x02 }),
            "HTTP/3 QPACK decoder should build Insert Count Increment");
        Http3QpackDecoderInstruction zeroIncrement;
        zeroIncrement.type = Http3QpackDecoderInstructionType::InsertCountIncrement;
        RequireStatus(BuildHttp3QpackDecoderInstruction(zeroIncrement),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK decoder should reject a zero Insert Count Increment");
    }

    void TestHttp3QpackInstructionApplication() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(256);
        Require(table.SetCapacity(128).IsOk(),
            "HTTP/3 QPACK instruction application fixture should set capacity");

        Http3QpackEncoderInstruction literal;
        literal.type = Http3QpackEncoderInstructionType::InsertWithLiteralName;
        literal.name.bytes = { 'x' };
        literal.fieldValue.bytes = { 'y' };
        auto encodedLiteral = BuildHttp3QpackEncoderInstruction(literal);
        Require(encodedLiteral.IsOk(),
            "HTTP/3 QPACK instruction application fixture should encode literal");
        auto parsedLiteral = ParseHttp3QpackEncoderInstruction(
            encodedLiteral.Value().data(), encodedLiteral.Value().size());
        Require(parsedLiteral.IsOk()
                && ApplyHttp3QpackEncoderInstruction(
                    parsedLiteral.Value().first, table).IsOk()
                && table.GetRelative(0).IsOk()
                && table.GetRelative(0).Value().name
                    == std::vector<std::uint8_t>({ 'x' }),
            "HTTP/3 QPACK instruction application should insert literal fields");

        Http3QpackEncoderInstruction dynamicName;
        dynamicName.type = Http3QpackEncoderInstructionType::InsertWithNameReference;
        dynamicName.value = 0;
        dynamicName.fieldValue.bytes = { 'z' };
        Require(ApplyHttp3QpackEncoderInstruction(dynamicName, table).IsOk()
                && table.GetRelative(0).Value().value
                    == std::vector<std::uint8_t>({ 'z' }),
            "HTTP/3 QPACK instruction application should resolve dynamic names");

        Http3QpackEncoderInstruction staticName;
        staticName.type = Http3QpackEncoderInstructionType::InsertWithNameReference;
        staticName.staticTable = true;
        staticName.value = 1;
        staticName.fieldValue.bytes = { '/' };
        Require(ApplyHttp3QpackEncoderInstruction(staticName, table).IsOk()
                && table.GetRelative(0).Value().name
                    == std::vector<std::uint8_t>({ ':', 'p', 'a', 't', 'h' }),
            "HTTP/3 QPACK instruction application should resolve static names");

        Http3QpackEncoderInstruction huffman;
        huffman.type = Http3QpackEncoderInstructionType::InsertWithLiteralName;
        huffman.name.bytes = { 'h' };
        huffman.fieldValue.huffmanEncoded = true;
        huffman.fieldValue.bytes = {
            0xF1, 0xE3, 0xC2, 0xE5, 0xF2, 0x3A,
            0x6B, 0xA0, 0xAB, 0x90, 0xF4, 0xFF
        };
        Require(ApplyHttp3QpackEncoderInstruction(huffman, table).IsOk()
                && table.GetRelative(0).Value().value
                    == std::vector<std::uint8_t>({
                        'w', 'w', 'w', '.', 'e', 'x', 'a', 'm',
                        'p', 'l', 'e', '.', 'c', 'o', 'm' }),
            "HTTP/3 QPACK instruction application should decode Huffman values");

        Http3QpackEncoderInstruction duplicate;
        duplicate.type = Http3QpackEncoderInstructionType::Duplicate;
        duplicate.value = 1;
        Require(ApplyHttp3QpackEncoderInstruction(duplicate, table).IsOk(),
            "HTTP/3 QPACK instruction application should duplicate entries");
        duplicate.value = 99;
        RequireStatus(ApplyHttp3QpackEncoderInstruction(duplicate, table),
            LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK instruction application should reject missing duplicates");

        Http3QpackSectionTracker tracker({ 4, 2 });
        Require(tracker.OpenSection(4, 2).IsOk(),
            "HTTP/3 QPACK decoder application fixture should open a section");
        Http3QpackDecoderInstruction increment;
        increment.type = Http3QpackDecoderInstructionType::InsertCountIncrement;
        increment.value = 2;
        Require(ApplyHttp3QpackDecoderInstruction(increment, tracker).IsOk()
                && tracker.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK decoder application should advance known inserts");
        Http3QpackDecoderInstruction acknowledgment;
        acknowledgment.type = Http3QpackDecoderInstructionType::SectionAcknowledgment;
        acknowledgment.value = 4;
        Require(ApplyHttp3QpackDecoderInstruction(acknowledgment, tracker).IsOk(),
            "HTTP/3 QPACK decoder application should acknowledge sections");
        RequireStatus(ApplyHttp3QpackDecoderInstruction(
                Http3QpackDecoderInstruction{
                    Http3QpackDecoderInstructionType::InsertCountIncrement, 0 },
                tracker),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK decoder application should reject zero increments");
        Require(tracker.OpenSection(6, 4).IsOk(),
            "HTTP/3 QPACK decoder application should open another section");
        Require(ApplyHttp3QpackDecoderInstruction(
                Http3QpackDecoderInstruction{
                    Http3QpackDecoderInstructionType::StreamCancellation, 6 },
                tracker).IsOk(),
            "HTTP/3 QPACK decoder application should cancel streams");
    }

    void TestHttp3QpackInstructionStreams() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        Http3QpackEncoderStream encoder(table, { 64, 4 });
        const std::uint8_t capacityFirst[] = { 0x3F };
        Require(encoder.Feed(capacityFirst, sizeof(capacityFirst)).IsOk()
                && encoder.Snapshot().bufferedBytes == 1,
            "HTTP/3 QPACK encoder stream should retain a split instruction");
        const std::uint8_t capacityRest[] = { 0x00 };
        Require(encoder.Feed(capacityRest, sizeof(capacityRest)).IsOk()
                && encoder.Snapshot().bufferedBytes == 0
                && table.Snapshot().capacity == 31,
            "HTTP/3 QPACK encoder stream should apply a completed instruction");
        Require(encoder.Finish().IsOk() && encoder.Snapshot().finished,
            "HTTP/3 QPACK encoder stream should finish without residual bytes");
        RequireStatus(encoder.Feed(nullptr, 0),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK encoder stream should reject data after FIN");

        Http3QpackDynamicTable incompleteTable(64);
        Http3QpackEncoderStream incomplete(incompleteTable);
        Require(incomplete.Feed(capacityFirst, sizeof(capacityFirst)).IsOk(),
            "HTTP/3 QPACK encoder stream should accept an incomplete prefix");
        RequireStatus(incomplete.Finish(), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK encoder stream should reject an incomplete FIN");
        Require(incomplete.Snapshot().terminal
                && incomplete.LastError().Code()
                    == LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK encoder stream should retain its terminal parse error");
        const auto encoderActions = incomplete.FailureActions();
        Require(encoderActions.IsOk()
                && encoderActions.Value().closeConnection
                && encoderActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(
                        Http3QpackStreamErrorCode::EncoderStreamError),
            "HTTP/3 QPACK encoder stream should map failure to close intent");
        incomplete.Reset();
        RequireStatus(incomplete.FailureActions(), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK encoder stream should require failure before mapping actions");

        Http3QpackSectionTracker tracker({ 4, 1 });
        Require(tracker.OpenSection(4, 2).IsOk(),
            "HTTP/3 QPACK decoder stream fixture should open a section");
        Http3QpackDecoderStream decoder(tracker, { 64, 4 });
        const std::uint8_t increment[] = { 0x02 };
        Require(decoder.Feed(increment, sizeof(increment)).IsOk()
                && tracker.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK decoder stream should apply insert increments");
        const std::uint8_t acknowledgment[] = { 0x84 };
        Require(decoder.Feed(acknowledgment, sizeof(acknowledgment)).IsOk()
                && tracker.Snapshot().outstandingSections == 0,
            "HTTP/3 QPACK decoder stream should apply acknowledgments");
        Require(decoder.Finish().IsOk() && decoder.Snapshot().finished,
            "HTTP/3 QPACK decoder stream should finish cleanly");

        Http3QpackSectionTracker invalidTracker;
        Http3QpackDecoderStream invalid(invalidTracker);
        const std::uint8_t zeroIncrement[] = { 0x00 };
        Require(invalid.Feed(zeroIncrement, sizeof(zeroIncrement)).IsOk(),
            "HTTP/3 QPACK decoder stream should retain a potentially split instruction");
        RequireStatus(invalid.Finish(), LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK decoder stream should reject a zero increment at FIN");
        const auto decoderActions = invalid.FailureActions();
        Require(decoderActions.IsOk()
                && decoderActions.Value().closeConnection
                && decoderActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(
                        Http3QpackStreamErrorCode::DecoderStreamError),
            "HTTP/3 QPACK decoder stream should map failure to close intent");
    }

    void TestHttp3QpackFieldSectionPrefix() {
        using namespace LikesProgram::Http;

        Http3QpackFieldSectionPrefix direct;
        direct.requiredInsertCount = 9;
        direct.base = 9;
        const auto encodedDirect = BuildHttp3QpackFieldSectionPrefix(direct, 100);
        Require(encodedDirect.IsOk()
                && encodedDirect.Value() == std::vector<std::uint8_t>({ 0x04, 0x00 }),
            "HTTP/3 QPACK field-section prefix should encode the RFC direct-base example");
        const auto parsedDirect = ParseHttp3QpackFieldSectionPrefix(
            encodedDirect.Value().data(), encodedDirect.Value().size(), 100, 10);
        Require(parsedDirect.IsOk()
                && parsedDirect.Value().first.requiredInsertCount == 9
                && parsedDirect.Value().first.base == 9
                && parsedDirect.Value().second == encodedDirect.Value().size(),
            "HTTP/3 QPACK field-section prefix should reconstruct the required insert count");

        Http3QpackFieldSectionPrefix postBase;
        postBase.requiredInsertCount = 9;
        postBase.base = 6;
        const auto encodedPostBase = BuildHttp3QpackFieldSectionPrefix(postBase, 100);
        Require(encodedPostBase.IsOk()
                && encodedPostBase.Value() == std::vector<std::uint8_t>({ 0x04, 0x82 }),
            "HTTP/3 QPACK field-section prefix should encode a signed base delta");
        const auto parsedPostBase = ParseHttp3QpackFieldSectionPrefix(
            encodedPostBase.Value().data(), encodedPostBase.Value().size(), 100, 10);
        Require(parsedPostBase.IsOk()
                && parsedPostBase.Value().first.requiredInsertCount == 9
                && parsedPostBase.Value().first.base == 6,
            "HTTP/3 QPACK field-section prefix should decode a signed base delta");

        const auto noReferences = BuildHttp3QpackFieldSectionPrefix({}, 0);
        Require(noReferences.IsOk()
                && noReferences.Value() == std::vector<std::uint8_t>({ 0x00, 0x00 }),
            "HTTP/3 QPACK field-section prefix should allow a zero-capacity literal section");
        const auto parsedNoReferences = ParseHttp3QpackFieldSectionPrefix(
            noReferences.Value().data(), noReferences.Value().size(), 0, 0);
        Require(parsedNoReferences.IsOk()
                && parsedNoReferences.Value().first.requiredInsertCount == 0
                && parsedNoReferences.Value().first.base == 0,
            "HTTP/3 QPACK zero required insert count should decode without table state");

        const std::uint8_t invalidEncoded[] = { 0x07, 0x00 };
        RequireStatus(ParseHttp3QpackFieldSectionPrefix(
                invalidEncoded, sizeof(invalidEncoded), 100, 10),
            LikesProgram::StatusCode::OutOfRange,
            "HTTP/3 QPACK field-section prefix should reject an encoded count beyond full range");
        const std::uint8_t invalidZero[] = { 0x01, 0x00 };
        RequireStatus(ParseHttp3QpackFieldSectionPrefix(
                invalidZero, sizeof(invalidZero), 100, 0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field-section prefix should reject an impossible wrapped zero");
        const std::uint8_t invalidBase[] = { 0x04, 0x89 };
        RequireStatus(ParseHttp3QpackFieldSectionPrefix(
                invalidBase, sizeof(invalidBase), 100, 10),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field-section prefix should reject a negative base");
        RequireStatus(BuildHttp3QpackFieldSectionPrefix(
                Http3QpackFieldSectionPrefix{ 1, 0 }, 0),
            LikesProgram::StatusCode::OutOfRange,
            "HTTP/3 QPACK field-section prefix should reject dynamic references with no table capacity");
        RequireStatus(ParseHttp3QpackFieldSectionPrefix(
                noReferences.Value().data(), 1, 0, 0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field-section prefix should reject a truncated base");
        const auto decompressionActions = MapHttp3QpackFieldSectionFailure(
            LikesProgram::Status::InvalidArgument(u"field section failure"));
        Require(decompressionActions.IsOk()
                && decompressionActions.Value().closeConnection
                && decompressionActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(
                        Http3QpackStreamErrorCode::DecompressionFailed),
            "HTTP/3 QPACK field-section failure should map to close intent");
        const auto requestStreamDecompressionActions =
            MapHttp3RequestStreamQpackFailure(
                LikesProgram::Status::InvalidArgument(u"request stream field failure"));
        Require(requestStreamDecompressionActions.IsOk()
                && requestStreamDecompressionActions.Value().closeConnection
                && requestStreamDecompressionActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(
                        Http3QpackStreamErrorCode::DecompressionFailed),
            "request-stream QPACK failure should reuse the close intent mapping");
        RequireStatus(MapHttp3QpackFieldSectionFailure(LikesProgram::Status()),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK field-section mapping should reject an OK status");
        RequireStatus(MapHttp3RequestStreamQpackFailure(LikesProgram::Status()),
            LikesProgram::StatusCode::FailedPrecondition,
            "request-stream QPACK mapping should reject an OK status");
    }

    void TestHttp3QpackFieldLines() {
        using namespace LikesProgram::Http;

        Http3QpackFieldLine indexed;
        indexed.type = Http3QpackFieldLineType::Indexed;
        indexed.index = 5;
        indexed.staticTable = true;
        auto encodedIndexed = BuildHttp3QpackFieldLine(indexed);
        Require(encodedIndexed.IsOk()
                && encodedIndexed.Value() == std::vector<std::uint8_t>({ 0xC5 }),
            "HTTP/3 QPACK indexed field-line should encode static references");
        auto parsedIndexed = ParseHttp3QpackFieldLine(
            encodedIndexed.Value().data(), encodedIndexed.Value().size());
        Require(parsedIndexed.IsOk()
                && parsedIndexed.Value().first.type == Http3QpackFieldLineType::Indexed
                && parsedIndexed.Value().first.staticTable
                && parsedIndexed.Value().first.index == 5,
            "HTTP/3 QPACK indexed field-line should preserve table and index flags");

        Http3QpackFieldLine postBase;
        postBase.type = Http3QpackFieldLineType::IndexedPostBase;
        postBase.index = 2;
        auto encodedPostBase = BuildHttp3QpackFieldLine(postBase);
        Require(encodedPostBase.IsOk()
                && encodedPostBase.Value() == std::vector<std::uint8_t>({ 0x12 }),
            "HTTP/3 QPACK post-base indexed field-line should encode its index");

        Http3QpackFieldLine nameReference;
        nameReference.type = Http3QpackFieldLineType::LiteralWithNameReference;
        nameReference.index = 3;
        nameReference.staticTable = true;
        nameReference.neverIndex = true;
        nameReference.value.bytes = { 'v' };
        auto encodedNameReference = BuildHttp3QpackFieldLine(nameReference);
        Require(encodedNameReference.IsOk()
                && encodedNameReference.Value()
                    == std::vector<std::uint8_t>({ 0x73, 0x01, 'v' }),
            "HTTP/3 QPACK name-reference field-line should encode N/T flags");
        auto parsedNameReference = ParseHttp3QpackFieldLine(
            encodedNameReference.Value().data(), encodedNameReference.Value().size());
        Require(parsedNameReference.IsOk()
                && parsedNameReference.Value().first.type
                    == Http3QpackFieldLineType::LiteralWithNameReference
                && parsedNameReference.Value().first.neverIndex
                && parsedNameReference.Value().first.staticTable
                && parsedNameReference.Value().first.value.bytes
                    == nameReference.value.bytes,
            "HTTP/3 QPACK name-reference field-line should round-trip");

        Http3QpackFieldLine postBaseName;
        postBaseName.type = Http3QpackFieldLineType::LiteralWithPostBaseNameReference;
        postBaseName.index = 2;
        postBaseName.neverIndex = true;
        postBaseName.value.bytes = { 'v' };
        auto encodedPostBaseName = BuildHttp3QpackFieldLine(postBaseName);
        Require(encodedPostBaseName.IsOk()
                && encodedPostBaseName.Value()
                    == std::vector<std::uint8_t>({ 0x0A, 0x01, 'v' }),
            "HTTP/3 QPACK post-base name-reference field-line should encode N");

        Http3QpackFieldLine literalName;
        literalName.type = Http3QpackFieldLineType::LiteralWithLiteralName;
        literalName.neverIndex = true;
        literalName.name.bytes = { 'x' };
        literalName.value.bytes = { 'y' };
        auto encodedLiteralName = BuildHttp3QpackFieldLine(literalName);
        Require(encodedLiteralName.IsOk()
                && encodedLiteralName.Value()
                    == std::vector<std::uint8_t>({ 0x31, 'x', 0x01, 'y' }),
            "HTTP/3 QPACK literal-name field-line should encode name and value");
        auto parsedLiteralName = ParseHttp3QpackFieldLine(
            encodedLiteralName.Value().data(), encodedLiteralName.Value().size());
        Require(parsedLiteralName.IsOk()
                && parsedLiteralName.Value().first.type
                    == Http3QpackFieldLineType::LiteralWithLiteralName
                && parsedLiteralName.Value().first.neverIndex
                && parsedLiteralName.Value().first.name.bytes
                    == literalName.name.bytes,
            "HTTP/3 QPACK literal-name field-line should round-trip opaque strings");

        const std::uint8_t truncated[] = { 0x73 };
        RequireStatus(ParseHttp3QpackFieldLine(truncated, sizeof(truncated)),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field-line should reject a truncated value literal");
        indexed.neverIndex = true;
        RequireStatus(BuildHttp3QpackFieldLine(indexed),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK indexed field-line should reject the N flag");
    }

    void TestHttp3QpackStaticTable() {
        using namespace LikesProgram::Http;

        Require(Http3QpackStaticTableSize() == 99,
            "HTTP/3 QPACK static table should expose all RFC entries");
        const auto authority = GetHttp3QpackStaticEntry(0);
        Require(authority.IsOk()
                && authority.Value().index == 0
                && authority.Value().name == ":authority"
                && authority.Value().value.empty(),
            "HTTP/3 QPACK static table should expose the first empty-value entry");
        const auto path = GetHttp3QpackStaticEntry(1);
        Require(path.IsOk() && path.Value().name == ":path"
                && path.Value().value == "/",
            "HTTP/3 QPACK static table should preserve pseudo-header values");
        const auto status = GetHttp3QpackStaticEntry(25);
        Require(status.IsOk() && status.Value().name == ":status"
                && status.Value().value == "200",
            "HTTP/3 QPACK static table should preserve response pseudo-headers");
        const auto policy = GetHttp3QpackStaticEntry(85);
        Require(policy.IsOk() && policy.Value().name == "content-security-policy"
                && policy.Value().value
                    == "script-src 'none'; object-src 'none'; base-uri 'none'",
            "HTTP/3 QPACK static table should preserve long field values");
        const auto last = GetHttp3QpackStaticEntry(98);
        Require(last.IsOk() && last.Value().name == "x-frame-options"
                && last.Value().value == "sameorigin",
            "HTTP/3 QPACK static table should expose the final RFC entry");
        RequireStatus(GetHttp3QpackStaticEntry(99),
            LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK static table should reject an out-of-range index");
    }

    void TestHttp3QpackFieldResolution() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        Require(table.SetCapacity(128).IsOk()
                && table.Insert({ ':', 'a' }, { 'o', 'n', 'e' }).IsOk()
                && table.Insert({ ':', 'b' }, { 't', 'w', 'o' }).IsOk(),
            "HTTP/3 QPACK field resolver fixture should populate dynamic entries");

        Http3QpackFieldLine staticPath;
        staticPath.type = Http3QpackFieldLineType::Indexed;
        staticPath.staticTable = true;
        staticPath.index = 1;
        auto resolvedStatic = ResolveHttp3QpackFieldLine(
            staticPath, Http3QpackFieldSectionPrefix{}, table);
        Require(resolvedStatic.IsOk()
                && resolvedStatic.Value().referencedTable
                && resolvedStatic.Value().staticTable
                && resolvedStatic.Value().referencedIndex == 1
                && resolvedStatic.Value().name.bytes
                    == std::vector<std::uint8_t>({ ':', 'p', 'a', 't', 'h' })
                && resolvedStatic.Value().value.bytes
                    == std::vector<std::uint8_t>({ '/' }),
            "HTTP/3 QPACK field resolver should resolve a static indexed field");

        Http3QpackFieldLine relative;
        relative.type = Http3QpackFieldLineType::Indexed;
        relative.index = 0;
        auto resolvedRelative = ResolveHttp3QpackFieldLine(
            relative, Http3QpackFieldSectionPrefix{ 2, 2 }, table);
        Require(resolvedRelative.IsOk()
                && !resolvedRelative.Value().staticTable
                && resolvedRelative.Value().referencedIndex == 1
                && resolvedRelative.Value().value.bytes
                    == std::vector<std::uint8_t>({ 't', 'w', 'o' }),
            "HTTP/3 QPACK field resolver should map Base-relative indices");

        Http3QpackFieldLine postBase;
        postBase.type = Http3QpackFieldLineType::IndexedPostBase;
        postBase.index = 1;
        auto resolvedPostBase = ResolveHttp3QpackFieldLine(
            postBase, Http3QpackFieldSectionPrefix{ 2, 0 }, table);
        Require(resolvedPostBase.IsOk()
                && resolvedPostBase.Value().referencedIndex == 1,
            "HTTP/3 QPACK field resolver should map post-Base indices");

        Http3QpackFieldLine dynamicName;
        dynamicName.type = Http3QpackFieldLineType::LiteralWithNameReference;
        dynamicName.index = 0;
        dynamicName.neverIndex = true;
        dynamicName.value.huffmanEncoded = true;
        dynamicName.value.bytes = { 0xAA };
        auto resolvedName = ResolveHttp3QpackFieldLine(
            dynamicName, Http3QpackFieldSectionPrefix{ 2, 2 }, table);
        Require(resolvedName.IsOk()
                && resolvedName.Value().neverIndex
                && resolvedName.Value().name.bytes
                    == std::vector<std::uint8_t>({ ':', 'b' })
                && resolvedName.Value().value.huffmanEncoded
                && resolvedName.Value().value.bytes
                    == dynamicName.value.bytes,
            "HTTP/3 QPACK field resolver should preserve opaque literal values");

        RequireStatus(ResolveHttp3QpackFieldLine(
                relative, Http3QpackFieldSectionPrefix{ 1, 0 }, table),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field resolver should reject relative-index underflow");
        RequireStatus(ResolveHttp3QpackFieldLine(
                relative, Http3QpackFieldSectionPrefix{ 1, 2 }, table),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field resolver should reject references beyond Required Insert Count");

        Http3QpackDynamicTable evicted(64);
        Require(evicted.SetCapacity(64).IsOk()
                && evicted.Insert({ 'a' }, { '1' }).IsOk()
                && evicted.Insert({ 'b' }, { '2' }).IsOk(),
            "HTTP/3 QPACK field resolver eviction fixture should populate entries");
        RequireStatus(ResolveHttp3QpackFieldLine(
                relative, Http3QpackFieldSectionPrefix{ 1, 1 }, evicted),
            LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK field resolver should reject an evicted dynamic entry");
    }

    void TestHttp3QpackFieldSectionParser() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        Require(table.SetCapacity(128).IsOk()
                && table.Insert({ ':', 'a' }, { 'o', 'n', 'e' }).IsOk()
                && table.Insert({ ':', 'b' }, { 't', 'w', 'o' }).IsOk(),
            "HTTP/3 QPACK section parser fixture should populate dynamic entries");

        auto prefix = BuildHttp3QpackFieldSectionPrefix({ 2, 2 }, 128);
        Http3QpackFieldLine staticPath;
        staticPath.type = Http3QpackFieldLineType::Indexed;
        staticPath.staticTable = true;
        staticPath.index = 1;
        auto encodedStatic = BuildHttp3QpackFieldLine(staticPath);
        Http3QpackFieldLine dynamicNewest;
        dynamicNewest.type = Http3QpackFieldLineType::Indexed;
        auto encodedDynamic = BuildHttp3QpackFieldLine(dynamicNewest);
        Http3QpackFieldLine literal;
        literal.type = Http3QpackFieldLineType::LiteralWithLiteralName;
        literal.name.bytes = { 'x' };
        literal.value.bytes = { 'y' };
        auto encodedLiteral = BuildHttp3QpackFieldLine(literal);
        Require(prefix.IsOk() && encodedStatic.IsOk()
                && encodedDynamic.IsOk() && encodedLiteral.IsOk(),
            "HTTP/3 QPACK section parser fixture should encode all parts");
        auto bytes = prefix.MoveValue();
        bytes.insert(bytes.end(), encodedStatic.Value().begin(), encodedStatic.Value().end());
        bytes.insert(bytes.end(), encodedDynamic.Value().begin(), encodedDynamic.Value().end());
        bytes.insert(bytes.end(), encodedLiteral.Value().begin(), encodedLiteral.Value().end());

        auto parsed = ParseHttp3QpackFieldSection(
            bytes.data(), bytes.size(), table);
        Require(parsed.IsOk()
                && parsed.Value().prefix.requiredInsertCount == 2
                && parsed.Value().fields.size() == 3
                && parsed.Value().fields[1].referencedIndex == 1
                && parsed.Value().fields[2].name.bytes
                    == std::vector<std::uint8_t>({ 'x' })
                && !parsed.Value().hasHuffman
                && parsed.Value().encodedBytes == bytes.size(),
            "HTTP/3 QPACK section parser should parse and resolve a complete section");

        auto staticPrefix = BuildHttp3QpackFieldSectionPrefix({}, 128);
        auto staticBytes = staticPrefix.MoveValue();
        staticBytes.insert(staticBytes.end(), encodedStatic.Value().begin(),
            encodedStatic.Value().end());
        auto parsedStatic = ParseHttp3QpackFieldSection(
            staticBytes.data(), staticBytes.size(), table);
        Require(parsedStatic.IsOk() && parsedStatic.Value().fields.size() == 1,
            "HTTP/3 QPACK section parser should allow static-only sections");

        auto blockedPrefix = BuildHttp3QpackFieldSectionPrefix({ 3, 3 }, 128);
        RequireStatus(ParseHttp3QpackFieldSection(
                blockedPrefix.Value().data(), blockedPrefix.Value().size(), table),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK section parser should expose blocked insert state");

        auto mismatchedPrefix = BuildHttp3QpackFieldSectionPrefix({ 2, 2 }, 128);
        auto mismatched = mismatchedPrefix.MoveValue();
        mismatched.insert(mismatched.end(), encodedStatic.Value().begin(),
            encodedStatic.Value().end());
        RequireStatus(ParseHttp3QpackFieldSection(
                mismatched.data(), mismatched.size(), table),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK section parser should validate Required Insert Count exactly");

        RequireStatus(ParseHttp3QpackFieldSection(bytes.data(), bytes.size(), table,
                Http3QpackFieldSectionLimits{ bytes.size() - 1, 256, 64 * 1024 }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK section parser should enforce encoded byte limits");
        RequireStatus(ParseHttp3QpackFieldSection(bytes.data(), bytes.size(), table,
                Http3QpackFieldSectionLimits{ 64 * 1024, 2, 64 * 1024 }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK section parser should enforce field-count limits");
        RequireStatus(ParseHttp3QpackFieldSection(bytes.data(), bytes.size(), table,
                Http3QpackFieldSectionLimits{ 64 * 1024, 256, 1 }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK section parser should enforce resolved byte limits");

        Http3QpackFieldLine huffman;
        huffman.type = Http3QpackFieldLineType::LiteralWithLiteralName;
        huffman.name.huffmanEncoded = true;
        huffman.name.bytes = { 0xAA };
        huffman.value.bytes = { 'v' };
        auto encodedHuffman = BuildHttp3QpackFieldLine(huffman);
        auto huffmanPrefix = BuildHttp3QpackFieldSectionPrefix({}, 128);
        Require(encodedHuffman.IsOk() && huffmanPrefix.IsOk(),
            "HTTP/3 QPACK section parser Huffman fixture should encode");
        auto huffmanBytes = huffmanPrefix.MoveValue();
        huffmanBytes.insert(huffmanBytes.end(), encodedHuffman.Value().begin(),
            encodedHuffman.Value().end());
        auto parsedHuffman = ParseHttp3QpackFieldSection(
            huffmanBytes.data(), huffmanBytes.size(), table);
        Require(parsedHuffman.IsOk() && parsedHuffman.Value().hasHuffman,
            "HTTP/3 QPACK section parser should expose opaque Huffman content");
    }

    void TestHttp3QpackFieldSectionDecoding() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        Http3QpackFieldLine literal;
        literal.type = Http3QpackFieldLineType::LiteralWithLiteralName;
        literal.name.huffmanEncoded = true;
        literal.name.bytes = {
            0xF1, 0xE3, 0xC2, 0xE5, 0xF2, 0x3A,
            0x6B, 0xA0, 0xAB, 0x90, 0xF4, 0xFF
        };
        literal.value.bytes = { 'v' };
        auto prefix = BuildHttp3QpackFieldSectionPrefix({}, 128);
        auto field = BuildHttp3QpackFieldLine(literal);
        Require(prefix.IsOk() && field.IsOk(),
            "HTTP/3 QPACK decoded section fixture should encode");
        auto bytes = prefix.MoveValue();
        bytes.insert(bytes.end(), field.Value().begin(), field.Value().end());
        auto parsed = ParseHttp3QpackFieldSection(
            bytes.data(), bytes.size(), table);
        Require(parsed.IsOk() && parsed.Value().hasHuffman,
            "HTTP/3 QPACK decoded section fixture should preserve encoded bytes");

        auto decoded = DecodeHttp3QpackFieldSection(parsed.Value());
        Require(decoded.IsOk()
                && !decoded.Value().hasHuffman
                && decoded.Value().fields.size() == 1
                && !decoded.Value().fields[0].name.huffmanEncoded
                && std::string(decoded.Value().fields[0].name.bytes.begin(),
                    decoded.Value().fields[0].name.bytes.end())
                    == "www.example.com"
                && decoded.Value().fields[0].value.bytes
                    == std::vector<std::uint8_t>({ 'v' })
                && decoded.Value().resolvedBytes == 16,
            "HTTP/3 QPACK field-section decoder should return bounded plain bytes");

        RequireStatus(DecodeHttp3QpackFieldSection(parsed.Value(), 15),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK field-section decoder should enforce its total output limit");

        auto invalid = parsed.Value();
        invalid.fields[0].name.bytes = { 0xFF, 0xFF, 0xFF, 0xFC };
        RequireStatus(DecodeHttp3QpackFieldSection(invalid),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK field-section decoder should reject an EOS symbol");
    }

    void TestHttp3QpackHeaderBlockValidation() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        auto prefix = BuildHttp3QpackFieldSectionPrefix({}, 128);
        Require(prefix.IsOk(),
            "HTTP/3 QPACK header block fixture should encode its prefix");
        auto bytes = prefix.MoveValue();
        for (const std::uint64_t index : { 17ULL, 23ULL, 1ULL }) {
            Http3QpackFieldLine field;
            field.type = Http3QpackFieldLineType::Indexed;
            field.staticTable = true;
            field.index = index;
            auto encoded = BuildHttp3QpackFieldLine(field);
            Require(encoded.IsOk(),
                "HTTP/3 QPACK header block fixture should encode static fields");
            bytes.insert(bytes.end(), encoded.Value().begin(), encoded.Value().end());
        }
        Http3QpackFieldLine huffman;
        huffman.type = Http3QpackFieldLineType::LiteralWithLiteralName;
        huffman.name.huffmanEncoded = true;
        huffman.name.bytes = {
            0xF1, 0xE3, 0xC2, 0xE5, 0xF2, 0x3A,
            0x6B, 0xA0, 0xAB, 0x90, 0xF4, 0xFF
        };
        huffman.value.bytes = { 'v' };
        auto encodedHuffman = BuildHttp3QpackFieldLine(huffman);
        Require(encodedHuffman.IsOk(),
            "HTTP/3 QPACK header block fixture should encode a Huffman field");
        bytes.insert(bytes.end(), encodedHuffman.Value().begin(),
            encodedHuffman.Value().end());

        auto parsed = ParseHttp3QpackFieldSection(
            bytes.data(), bytes.size(), table);
        Require(parsed.IsOk(),
            "HTTP/3 QPACK header block fixture should parse");
        auto request = DecodeHttp3QpackHeaderBlock(parsed.Value());
        Require(request.IsOk()
                && request.Value().size() == 4
                && request.Value()[0].name == ":method"
                && request.Value()[0].value == "GET"
                && request.Value()[1].name == ":scheme"
                && request.Value()[2].name == ":path"
                && request.Value()[3].name == "www.example.com",
            "HTTP/3 QPACK header block should decode and validate a request");

        RequireStatus(DecodeHttp3QpackHeaderBlock(
                parsed.Value(), HttpHeaderBlockOptions{ false, false }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK request pseudo-headers should fail response validation");

        auto invalid = parsed.Value();
        invalid.fields.back().name.huffmanEncoded = false;
        invalid.fields.back().name.bytes = { 'B', 'a', 'd' };
        RequireStatus(DecodeHttp3QpackHeaderBlock(invalid),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK header block should reuse lowercase field validation");
    }

    void TestHttp3QpackSectionTracker() {
        using namespace LikesProgram::Http;

        Http3QpackSectionTracker tracker({ 3, 1 });
        Require(tracker.SetKnownInsertCount(2).IsOk(),
            "HTTP/3 QPACK section tracker should set its initial insert count");
        Require(tracker.OpenSection(4, 3).IsOk()
                && tracker.OpenSection(4, 4).IsOk(),
            "HTTP/3 QPACK section tracker should queue sections on one stream");
        Require(tracker.Snapshot().outstandingSections == 2
                && tracker.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK section tracker should count blocked streams once");
        RequireStatus(tracker.OpenSection(6, 5),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK section tracker should enforce blocked stream limits");

        Require(tracker.SetKnownInsertCount(3).IsOk()
                && tracker.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK section tracker should unblock only satisfied sections");
        Require(tracker.AcknowledgeSection(4).IsOk()
                && tracker.Snapshot().outstandingSections == 1
                && tracker.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK section tracker should acknowledge in stream order");
        Require(tracker.SetKnownInsertCount(4).IsOk()
                && tracker.Snapshot().blockedStreams == 0
                && tracker.Snapshot().pendingUnblockedStreams == 1,
            "HTTP/3 QPACK section tracker should refresh blocked state on inserts");
        auto unblocked = tracker.TakeUnblockedStreams();
        Require(unblocked.IsOk()
                && unblocked.Value() == std::vector<std::uint64_t>({ 4 })
                && tracker.Snapshot().pendingUnblockedStreams == 0,
            "HTTP/3 QPACK section tracker should take each runnable stream once");
        auto duplicateTake = tracker.TakeUnblockedStreams();
        Require(duplicateTake.IsOk() && duplicateTake.Value().empty(),
            "HTTP/3 QPACK section tracker should not repeat taken wakeups");
        Require(tracker.AcknowledgeSection(4).IsOk()
                && tracker.Snapshot().acknowledgedSections == 2,
            "HTTP/3 QPACK section tracker should record section acknowledgments");
        RequireStatus(tracker.AcknowledgeSection(4),
            LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK section tracker should reject duplicate acknowledgments");

        Require(tracker.OpenSection(6, 5).IsOk()
                && tracker.CancelStream(6).IsOk()
                && tracker.Snapshot().outstandingSections == 0,
            "HTTP/3 QPACK section tracker should cancel all stream sections");
        RequireStatus(tracker.CancelStream(6), LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK section tracker should reject duplicate cancellation");

        Http3QpackSectionTracker orderedWakeups({ 4, 2 });
        Require(orderedWakeups.SetKnownInsertCount(1).IsOk()
                && orderedWakeups.OpenSection(8, 3).IsOk()
                && orderedWakeups.OpenSection(4, 2).IsOk()
                && orderedWakeups.SetKnownInsertCount(3).IsOk(),
            "HTTP/3 QPACK section tracker should collect runnable transitions");
        auto ordered = orderedWakeups.TakeUnblockedStreams();
        Require(ordered.IsOk()
                && ordered.Value() == std::vector<std::uint64_t>({ 4, 8 }),
            "HTTP/3 QPACK wakeup batch should use deterministic stream order");
        Require(orderedWakeups.OpenSection(6, 4).IsOk()
                && orderedWakeups.SetKnownInsertCount(4).IsOk()
                && orderedWakeups.CancelStream(6).IsOk(),
            "HTTP/3 QPACK cancellation should retire a pending wakeup");
        auto cancelledWakeup = orderedWakeups.TakeUnblockedStreams();
        Require(cancelledWakeup.IsOk() && cancelledWakeup.Value().empty(),
            "HTTP/3 QPACK tracker should not wake a cancelled stream");
        RequireStatus(tracker.SetKnownInsertCount(3),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK section tracker should reject insert-count rollback");

        const std::uint64_t maxQuic = (std::uint64_t{ 1 } << 62) - 1;
        RequireStatus(tracker.OpenSection(maxQuic + 1, 0),
            LikesProgram::StatusCode::OutOfRange,
            "HTTP/3 QPACK section tracker should reject oversized stream ids");
        Http3QpackSectionTracker moved(std::move(tracker));
        tracker.Reset();
        RequireStatus(tracker.SetKnownInsertCount(1),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK section tracker should reject moved-from use");
        RequireStatus(tracker.TakeUnblockedStreams(),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK section tracker should reject moved-from wakeup access");
        moved.Reset();
        Require(moved.Snapshot().outstandingSections == 0
                && moved.Snapshot().pendingUnblockedStreams == 0,
            "HTTP/3 QPACK section tracker reset should clear sections and wakeups");
    }

    void TestHttp3QpackSectionBudgetBinding() {
        using namespace LikesProgram::Http;

        Http3QpackResourceBudget budget(Http3QpackLimits{ 64, 1, 128 });
        Http3QpackSectionTracker tracker({ 8, 2 });
        RequireStatus(tracker.AttachResourceBudget(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK tracker should reject a null resource budget");
        Require(budget.OpenBlockedStream(12).IsOk(),
            "HTTP/3 QPACK budget binding fixture should reserve a stream");
        RequireStatus(tracker.AttachResourceBudget(&budget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK tracker should reject a non-empty blocked-stream budget");
        Require(budget.CloseBlockedStream(12).IsOk()
                && tracker.AttachResourceBudget(&budget).IsOk()
                && tracker.AttachResourceBudget(&budget).IsOk()
                && tracker.HasResourceBudget(),
            "HTTP/3 QPACK tracker should attach one empty budget idempotently");

        Require(tracker.SetKnownInsertCount(1).IsOk()
                && tracker.OpenSection(4, 2).IsOk()
                && tracker.OpenSection(4, 3).IsOk()
                && budget.Snapshot().blockedStreams == 1
                && budget.IsBlockedStreamOpen(4),
            "HTTP/3 QPACK sections on one stream should share one budget slot");
        const auto beforeRejectedOpen = tracker.Snapshot();
        RequireStatus(tracker.OpenSection(8, 4),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK budget should reject a second blocked stream");
        const auto afterRejectedOpen = tracker.Snapshot();
        Require(afterRejectedOpen.knownInsertCount
                    == beforeRejectedOpen.knownInsertCount
                && afterRejectedOpen.outstandingSections
                    == beforeRejectedOpen.outstandingSections
                && afterRejectedOpen.blockedStreams
                    == beforeRejectedOpen.blockedStreams
                && afterRejectedOpen.pendingUnblockedStreams
                    == beforeRejectedOpen.pendingUnblockedStreams
                && budget.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK budget rejection should leave tracker state unchanged");

        Require(tracker.SetKnownInsertCount(2).IsOk()
                && tracker.AcknowledgeSection(4).IsOk()
                && tracker.Snapshot().blockedStreams == 1
                && budget.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK budget should remain reserved while one section blocks");
        Require(tracker.SetKnownInsertCount(3).IsOk()
                && tracker.Snapshot().blockedStreams == 0
                && tracker.Snapshot().pendingUnblockedStreams == 1
                && budget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK insert progress should atomically release the budget slot");
        Require(tracker.OpenSection(4, 4).IsOk()
                && tracker.Snapshot().pendingUnblockedStreams == 0
                && budget.Snapshot().blockedStreams == 1
                && tracker.CancelStream(4).IsOk()
                && budget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK cancellation should release a rebound stream slot");

        Require(tracker.OpenSection(8, 5).IsOk()
                && tracker.AcknowledgeSection(8).IsOk()
                && tracker.Snapshot().pendingUnblockedStreams == 0
                && budget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK terminal acknowledgment should release without a wakeup");
        Require(tracker.OpenSection(12, 6).IsOk()
                && budget.Snapshot().blockedStreams == 1,
            "HTTP/3 QPACK reset fixture should reserve one slot");
        tracker.Reset();
        Require(tracker.HasResourceBudget()
                && tracker.Snapshot().outstandingSections == 0
                && budget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK reset should release slots and retain the attachment");

        Http3QpackSectionTracker lateAttach;
        Require(lateAttach.OpenSection(0, 0).IsOk(),
            "HTTP/3 QPACK late-attach fixture should open one section");
        RequireStatus(lateAttach.AttachResourceBudget(&budget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK budget should attach before section tracking");

        Http3QpackResourceBudget lifetimeBudget(
            Http3QpackLimits{ 64, 1, 128 });
        {
            Http3QpackSectionTracker lifetimeTracker;
            Require(lifetimeTracker.AttachResourceBudget(&lifetimeBudget).IsOk()
                    && lifetimeTracker.OpenSection(16, 1).IsOk()
                    && lifetimeBudget.Snapshot().blockedStreams == 1,
                "HTTP/3 QPACK lifetime fixture should reserve one slot");
        }
        Require(lifetimeBudget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK tracker destruction should release budget slots");
    }

    void TestHttp3QpackDynamicTable() {
        using namespace LikesProgram::Http;

        Http3QpackDynamicTable table(128);
        Require(table.SetCapacity(64).IsOk(),
            "HTTP/3 QPACK dynamic table should accept a configured capacity");
        const auto first = table.Insert({ 'a' }, { 'b' });
        Require(first.IsOk() && first.Value() == 0,
            "HTTP/3 QPACK dynamic table should assign absolute index zero");
        const auto second = table.Insert({ 'c' }, { 'd' });
        Require(second.IsOk() && second.Value() == 1
                && table.Snapshot().entryCount == 1
                && table.Snapshot().droppedEntries == 1,
            "HTTP/3 QPACK dynamic table should evict oldest entries at capacity");
        RequireStatus(table.GetAbsolute(0), LikesProgram::StatusCode::NotFound,
            "HTTP/3 QPACK dynamic table should not resolve an evicted absolute entry");
        const auto newest = table.GetRelative(0);
        Require(newest.IsOk() && newest.Value().absoluteIndex == 1
                && newest.Value().name == std::vector<std::uint8_t>({ 'c' }),
            "HTTP/3 QPACK dynamic table should resolve relative index zero as newest");

        Require(table.SetCapacity(32).IsOk()
                && table.Snapshot().entryCount == 0
                && table.Snapshot().bytes == 0,
            "HTTP/3 QPACK dynamic table should evict on capacity reduction");
        RequireStatus(table.Insert({ 'x' }, { 'y' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK dynamic table should reject entries larger than capacity");
        RequireStatus(table.SetCapacity(129), LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK dynamic table should enforce the configured maximum");

        Require(table.SetCapacity(128).IsOk()
                && table.Insert({ 'n' }, { 'v' }).IsOk()
                && table.Duplicate(0).IsOk()
                && table.Snapshot().entryCount == 2
                && table.Snapshot().insertCount == 4,
            "HTTP/3 QPACK dynamic table should duplicate a relative entry");
        table.Reset();
        Require(table.Snapshot().capacity == 0
                && table.Snapshot().entryCount == 0
                && table.MaxCapacity() == 128,
            "HTTP/3 QPACK dynamic table reset should retain configured maximum");
    }

    void TestHttp3QpackDynamicTableBudgetBinding() {
        using namespace LikesProgram::Http;

        Http3QpackResourceBudget budget(Http3QpackLimits{ 64, 2, 128 });
        Http3QpackDynamicTable table(128);
        RequireStatus(table.AttachResourceBudget(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QPACK dynamic table should reject a null budget");
        Require(budget.SetDynamicTableCapacity(16).IsOk(),
            "HTTP/3 QPACK table budget fixture should set capacity");
        RequireStatus(table.AttachResourceBudget(&budget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK table should reject a non-empty dynamic budget");
        Require(budget.SetDynamicTableCapacity(0).IsOk()
                && table.AttachResourceBudget(&budget).IsOk()
                && table.AttachResourceBudget(&budget).IsOk()
                && table.HasResourceBudget(),
            "HTTP/3 QPACK table should attach one empty budget idempotently");

        Require(table.SetCapacity(64).IsOk()
                && budget.Snapshot().dynamicTableCapacity == 64,
            "HTTP/3 QPACK table capacity should synchronize to its budget");
        Require(table.Insert({ 'a' }, { '1' }).IsOk()
                && table.Insert({ 'b' }, { '2' }).IsOk()
                && table.Snapshot().entryCount == 1
                && table.Snapshot().bytes == 34
                && table.Snapshot().droppedEntries == 1
                && budget.Snapshot().dynamicTableBytes == 34,
            "HTTP/3 QPACK table eviction should update one shared byte ledger");

        const auto beforeRejectedCapacity = table.Snapshot();
        const auto beforeRejectedBudget = budget.Snapshot();
        RequireStatus(table.SetCapacity(65),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QPACK budget should enforce a stricter table capacity");
        const auto afterRejectedCapacity = table.Snapshot();
        const auto afterRejectedBudget = budget.Snapshot();
        Require(afterRejectedCapacity.capacity == beforeRejectedCapacity.capacity
                && afterRejectedCapacity.bytes == beforeRejectedCapacity.bytes
                && afterRejectedCapacity.entryCount
                    == beforeRejectedCapacity.entryCount
                && afterRejectedCapacity.droppedEntries
                    == beforeRejectedCapacity.droppedEntries
                && afterRejectedCapacity.insertCount
                    == beforeRejectedCapacity.insertCount
                && afterRejectedBudget.dynamicTableCapacity
                    == beforeRejectedBudget.dynamicTableCapacity
                && afterRejectedBudget.dynamicTableBytes
                    == beforeRejectedBudget.dynamicTableBytes,
            "HTTP/3 QPACK budget rejection should preserve both table ledgers");

        Require(table.SetCapacity(32).IsOk()
                && table.Snapshot().entryCount == 0
                && table.Snapshot().bytes == 0
                && table.Snapshot().droppedEntries == 2
                && budget.Snapshot().dynamicTableCapacity == 32
                && budget.Snapshot().dynamicTableBytes == 0,
            "HTTP/3 QPACK capacity reduction should release evicted budget bytes");
        Require(table.SetCapacity(64).IsOk()
                && table.Insert({ 'n' }, { 'v' }).IsOk()
                && table.Duplicate(0).IsOk()
                && table.Snapshot().entryCount == 1
                && table.Snapshot().bytes == 34
                && budget.Snapshot().dynamicTableBytes == 34,
            "HTTP/3 QPACK duplicate should use the bound eviction transaction");
        table.Reset();
        Require(table.HasResourceBudget()
                && table.Snapshot().capacity == 0
                && table.Snapshot().bytes == 0
                && budget.Snapshot().dynamicTableCapacity == 0
                && budget.Snapshot().dynamicTableBytes == 0,
            "HTTP/3 QPACK table reset should release and retain its binding");

        Http3QpackDynamicTable lateAttach(16);
        Require(lateAttach.SetCapacity(1).IsOk(),
            "HTTP/3 QPACK late table attachment fixture should mutate capacity");
        RequireStatus(lateAttach.AttachResourceBudget(&budget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QPACK table budget should attach before mutation");

        Http3QpackResourceBudget sharedBudget(
            Http3QpackLimits{ 64, 1, 128 });
        Http3QpackSectionTracker tracker;
        Require(tracker.AttachResourceBudget(&sharedBudget).IsOk()
                && tracker.OpenSection(4, 1).IsOk(),
            "HTTP/3 QPACK shared budget fixture should reserve a blocked stream");
        {
            Http3QpackDynamicTable lifetimeTable(64);
            Require(lifetimeTable.AttachResourceBudget(&sharedBudget).IsOk()
                    && lifetimeTable.SetCapacity(64).IsOk()
                    && lifetimeTable.Insert({ 'x' }, { 'y' }).IsOk()
                    && sharedBudget.Snapshot().blockedStreams == 1
                    && sharedBudget.Snapshot().dynamicTableBytes == 34,
                "HTTP/3 QPACK budget dimensions should support separate owners");
        }
        Require(sharedBudget.Snapshot().blockedStreams == 1
                && sharedBudget.Snapshot().dynamicTableCapacity == 0
                && sharedBudget.Snapshot().dynamicTableBytes == 0
                && tracker.CancelStream(4).IsOk()
                && sharedBudget.Snapshot().blockedStreams == 0,
            "HTTP/3 QPACK table destruction should preserve blocked-stream usage");

        Http3QpackResourceBudget replacedBudget(
            Http3QpackLimits{ 64, 1, 128 });
        Http3QpackResourceBudget movedBudget(
            Http3QpackLimits{ 64, 1, 128 });
        Http3QpackDynamicTable replaced(64);
        Http3QpackDynamicTable moved(64);
        Require(replaced.AttachResourceBudget(&replacedBudget).IsOk()
                && replaced.SetCapacity(64).IsOk()
                && replaced.Insert({ 'r' }, { '1' }).IsOk()
                && moved.AttachResourceBudget(&movedBudget).IsOk()
                && moved.SetCapacity(64).IsOk()
                && moved.Insert({ 'm' }, { '2' }).IsOk(),
            "HTTP/3 QPACK move-assignment fixture should populate both tables");
        replaced = std::move(moved);
        Require(replacedBudget.Snapshot().dynamicTableCapacity == 0
                && replacedBudget.Snapshot().dynamicTableBytes == 0
                && movedBudget.Snapshot().dynamicTableCapacity == 64
                && movedBudget.Snapshot().dynamicTableBytes == 34,
            "HTTP/3 QPACK move assignment should release replaced budget usage");
        replaced.Reset();
        Require(movedBudget.Snapshot().dynamicTableCapacity == 0
                && movedBudget.Snapshot().dynamicTableBytes == 0,
            "HTTP/3 QPACK moved table should retain and release its budget");
    }

    void TestHttp3ControlStream() {
        using namespace LikesProgram::Http;
        using LikesProgram::StatusCode;
        auto makeFrame = [](std::uint64_t type,
            std::vector<std::uint8_t> payload) {
            Http3Frame frame;
            frame.type = type;
            frame.payload = std::move(payload);
            return frame;
        };

        const auto streamType = BuildHttp3ControlStreamType();
        Require(streamType.IsOk()
                && streamType.Value() == std::vector<std::uint8_t>{ 0 },
            "HTTP/3 control stream type should use the zero QUIC varint");

        const std::vector<Http3Setting> settings{
            { 0x1, 1024 }, { 0x7, 8 }
        };
        const auto settingsPayload = BuildHttp3Settings(settings);
        Require(settingsPayload.IsOk(),
            "HTTP/3 SETTINGS payload should build");
        const auto initialControlBytes =
            BuildHttp3ControlStreamInitialBytes(settings);
        Require(initialControlBytes.IsOk()
                && initialControlBytes.Value().size() > streamType.Value().size()
                && std::equal(streamType.Value().begin(), streamType.Value().end(),
                    initialControlBytes.Value().begin()),
            "HTTP/3 control initial bytes should prefix the stream type");
        const auto initialFrame = ParseHttp3Frame(
            initialControlBytes.Value().data() + streamType.Value().size(),
            initialControlBytes.Value().size() - streamType.Value().size());
        Require(initialFrame.IsOk()
                && initialFrame.Value().type
                    == static_cast<std::uint64_t>(Http3FrameType::Settings),
            "HTTP/3 control initial bytes should contain SETTINGS");
        RequireStatus(BuildHttp3ControlStreamInitialBytes({ { 1, 2 }, { 1, 3 } }),
            StatusCode::InvalidArgument,
            "HTTP/3 control initial bytes should reject duplicate settings");
        const auto parsedSettings = ParseHttp3Settings(settingsPayload.Value());
        Require(parsedSettings.IsOk()
                && parsedSettings.Value().size() == settings.size()
                && parsedSettings.Value()[0].id == settings[0].id
                && parsedSettings.Value()[0].value == settings[0].value
                && parsedSettings.Value()[1].id == settings[1].id
                && parsedSettings.Value()[1].value == settings[1].value,
            "HTTP/3 SETTINGS payload should round trip QUIC varints");
        RequireStatus(BuildHttp3Settings({ { 1, 2 }, { 1, 3 } }),
            StatusCode::InvalidArgument,
            "HTTP/3 SETTINGS builder should reject duplicate identifiers");
        const std::vector<std::uint8_t> duplicatePayload{ 1, 2, 1, 3 };
        RequireStatus(ParseHttp3Settings(duplicatePayload),
            StatusCode::InvalidArgument,
            "HTTP/3 SETTINGS parser should reject duplicate identifiers");

        Http3ControlStream control;
        const auto settingsFrame = makeFrame(
            static_cast<std::uint64_t>(Http3FrameType::Settings),
            settingsPayload.Value());
        RequireStatus(control.Feed(settingsFrame), StatusCode::FailedPrecondition,
            "HTTP/3 control stream should require its unidirectional type first");
        Require(control.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && control.AcceptStreamType(kHttp3ControlStreamType).GetStatus().Code()
                    == StatusCode::FailedPrecondition,
            "HTTP/3 control stream should accept its type only once");
        Http3Frame dataFrame = makeFrame(
            static_cast<std::uint64_t>(Http3FrameType::Data), { 1 });
        RequireStatus(control.Feed(dataFrame), StatusCode::InvalidArgument,
            "HTTP/3 control stream should require SETTINGS as its first frame");

        control.Reset();
        Require(control.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && control.Feed(settingsFrame).IsOk()
                && control.State() == Http3ControlStreamState::Open
                && control.Settings() == settings,
            "HTTP/3 control stream should enter Open after one SETTINGS frame");
        RequireStatus(control.Feed(settingsFrame), StatusCode::InvalidArgument,
            "HTTP/3 control stream should reject duplicate SETTINGS");

        Http3ControlStream extensionStream;
        Require(extensionStream.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && extensionStream.Feed(settingsFrame).IsOk()
                && extensionStream.Feed(makeFrame(0x1F, { 9, 9 })).IsOk(),
            "HTTP/3 control stream should pass unknown extension frames after SETTINGS");
        RequireStatus(extensionStream.Feed(dataFrame), StatusCode::InvalidArgument,
            "HTTP/3 control stream should reject DATA frames");

        const auto goaway8 = BuildHttp3VarInt(8);
        const auto goaway4 = BuildHttp3VarInt(4);
        const auto goaway9 = BuildHttp3VarInt(9);
        Http3ControlStream goawayStream;
        Require(goawayStream.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && goawayStream.Feed(settingsFrame).IsOk()
                && goawayStream.Feed(makeFrame(
                    static_cast<std::uint64_t>(Http3FrameType::Goaway),
                    goaway8.Value())).IsOk()
                && goawayStream.Feed(makeFrame(
                    static_cast<std::uint64_t>(Http3FrameType::Goaway),
                    goaway4.Value())).IsOk(),
            "HTTP/3 control stream should accept decreasing GOAWAY ids");
        RequireStatus(goawayStream.Feed(makeFrame(
                static_cast<std::uint64_t>(Http3FrameType::Goaway), goaway9.Value())),
            StatusCode::InvalidArgument,
            "HTTP/3 control stream should reject increasing GOAWAY ids");

        const auto push3 = BuildHttp3VarInt(3);
        const auto push4 = BuildHttp3VarInt(4);
        const auto push2 = BuildHttp3VarInt(2);
        Http3ControlStream pushStream;
        Require(pushStream.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && pushStream.Feed(settingsFrame).IsOk()
                && pushStream.Feed(makeFrame(
                    static_cast<std::uint64_t>(Http3FrameType::MaxPushId),
                    push3.Value())).IsOk()
                && pushStream.Feed(makeFrame(
                    static_cast<std::uint64_t>(Http3FrameType::MaxPushId),
                    push4.Value())).IsOk()
                && pushStream.Feed(makeFrame(
                    static_cast<std::uint64_t>(Http3FrameType::CancelPush),
                    push2.Value())).IsOk(),
            "HTTP/3 control stream should track MAX_PUSH_ID and CANCEL_PUSH");
        const auto pushSnapshot = pushStream.Snapshot();
        Require(pushSnapshot.maxPushIdReceived && pushSnapshot.maxPushId == 4
                && pushSnapshot.cancelPushCount == 1
                && pushSnapshot.lastCancelPushId == 2,
            "HTTP/3 control stream snapshot should expose control identifiers");
        RequireStatus(pushStream.Feed(makeFrame(
                static_cast<std::uint64_t>(Http3FrameType::MaxPushId),
                push2.Value())),
            StatusCode::InvalidArgument,
            "HTTP/3 control stream should reject decreasing MAX_PUSH_ID");

        Http3ControlStream wrongType;
        RequireStatus(wrongType.AcceptStreamType(0x02), StatusCode::InvalidArgument,
            "HTTP/3 control stream should reject a non-control stream type");

        Http3ControlStream limited(1);
        Require(limited.AcceptStreamType(kHttp3ControlStreamType).IsOk(),
            "HTTP/3 limited control stream should accept its stream type");
        RequireStatus(limited.Feed(settingsFrame), StatusCode::ResourceExhausted,
            "HTTP/3 control stream should enforce its SETTINGS count limit");

        Http3ControlStream moved(std::move(extensionStream));
        Require(extensionStream.State() == Http3ControlStreamState::Failed
                && moved.Snapshot().settingsReceived,
            "HTTP/3 control stream should expose moved-from and moved-to state");
        moved.Reset();
        Require(moved.AcceptStreamType(kHttp3ControlStreamType).IsOk()
                && moved.Feed(settingsFrame).IsOk(),
            "HTTP/3 control stream Reset should permit a fresh handshake");

        const auto encodedSettingsFrame = BuildHttp3Frame(settingsFrame);
        const auto encodedExtensionFrame = BuildHttp3Frame(makeFrame(0x1F, { 9, 9 }));
        Require(encodedSettingsFrame.IsOk() && encodedExtensionFrame.IsOk(),
            "HTTP/3 control wire fixtures should build complete frames");
        Http3ControlStreamWireDecoder wire;
        std::vector<std::uint8_t> wireBytes = streamType.Value();
        wireBytes.insert(wireBytes.end(), encodedSettingsFrame.Value().begin(),
            encodedSettingsFrame.Value().end());
        wireBytes.insert(wireBytes.end(), encodedExtensionFrame.Value().begin(),
            encodedExtensionFrame.Value().end());
        for (const auto byte : wireBytes) {
            Require(wire.Feed(&byte, 1).IsOk(),
                "HTTP/3 control wire decoder should hold partial type and frames");
        }
        Require(wire.PendingBytes() == 0
                && wire.Snapshot().settingsReceived
                && wire.Snapshot().settingsCount == settings.size()
                && wire.Settings() == settings,
            "HTTP/3 control wire decoder should map chunked type and SETTINGS");
        RequireStatus(wire.Finish(), StatusCode::InvalidArgument,
            "HTTP/3 control wire decoder should reject critical-stream FIN");
        Require(!wire.LastError().IsOk(),
            "HTTP/3 control wire decoder should expose critical-stream close failure");
        const auto criticalActions = wire.FailureActions();
        Require(criticalActions.IsOk()
                && criticalActions.Value().closeConnection
                && criticalActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::ClosedCriticalStream),
            "HTTP/3 control wire decoder should map FIN to connection-close intent");
        wire.Reset();
        Require(wire.Feed(wireBytes).IsOk()
                && wire.Snapshot().settingsReceived,
            "HTTP/3 control wire decoder Reset should permit a fresh stream");
        RequireStatus(wire.FailureActions(), StatusCode::FailedPrecondition,
            "HTTP/3 control wire decoder should require a failure before mapping actions");

        Http3ControlStreamWireDecoder wrongWire;
        RequireStatus(wrongWire.Feed(std::vector<std::uint8_t>{ 0x02 }),
            StatusCode::InvalidArgument,
            "HTTP/3 control wire decoder should reject a non-control stream type");
        const auto wrongActions = wrongWire.FailureActions();
        Require(wrongActions.IsOk()
                && wrongActions.Value().closeConnection
                && wrongActions.Value().quicErrorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::GeneralProtocolError),
            "HTTP/3 control wire decoder should map protocol failure to close intent");
        Http3ControlStreamWireDecoder boundedWire(64, { 64, 1 });
        RequireStatus(boundedWire.Feed(wireBytes), StatusCode::ResourceExhausted,
            "HTTP/3 control wire decoder should enforce frame payload limits");
        Http3ControlStreamWireDecoder truncatedWire;
        std::vector<std::uint8_t> partialWire{ streamType.Value().front(),
            encodedSettingsFrame.Value().front() };
        Require(truncatedWire.Feed(partialWire).IsOk(),
            "HTTP/3 control wire decoder should wait for a complete SETTINGS frame");
        RequireStatus(truncatedWire.Finish(), StatusCode::InvalidArgument,
            "HTTP/3 control wire decoder should reject FIN with an incomplete frame");
    }

    void TestHttpHeaderBlockValidation() {
        using LikesProgram::Http::HttpHeader;
        const std::vector<HttpHeader> request{
            { ":method", "GET" },
            { ":scheme", "https" },
            { ":authority", "example.test" },
            { ":path", "/" },
            { "accept", "*/*" }
        };
        Require(LikesProgram::Http::ValidateHttp2HeaderBlock(request).IsOk(),
            "HTTP/2 request header block should validate");
        Require(LikesProgram::Http::ValidateHttp3HeaderBlock(request).IsOk(),
            "HTTP/3 request header block should validate");

        const std::vector<HttpHeader> response{
            { ":status", "200" },
            { "content-type", "text/plain" }
        };
        Require(LikesProgram::Http::ValidateHttp2HeaderBlock(
            response, { false, false }).IsOk(),
            "HTTP/2 response header block should validate");
        Require(LikesProgram::Http::ValidateHttp3HeaderBlock(
            response, { false, false }).IsOk(),
            "HTTP/3 response header block should validate");

        const std::vector<HttpHeader> trailer{ { "grpc-status", "0" } };
        Require(LikesProgram::Http::ValidateHttp2HeaderBlock(
            trailer, { false, true }).IsOk(),
            "HTTP/2 trailer block should validate");

        auto expectInvalid = [](std::vector<HttpHeader> headers,
            LikesProgram::Http::HttpHeaderBlockOptions options = {}) {
            Require(!LikesProgram::Http::ValidateHttp2HeaderBlock(headers, options).IsOk(),
                "invalid HTTP/2 header block should be rejected");
            Require(!LikesProgram::Http::ValidateHttp3HeaderBlock(headers, options).IsOk(),
                "invalid HTTP/3 header block should be rejected");
        };
        expectInvalid({ { "Host", "example.test" } });
        expectInvalid({ { "", "value" } });
        expectInvalid({ { ":method", "GET" }, { "accept", "*/*" }, { ":path", "/" },
            { ":scheme", "https" } });
        expectInvalid({ { ":method", "GET" }, { ":method", "POST" },
            { ":scheme", "https" }, { ":path", "/" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { "connection", "keep-alive" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { "te", "gzip" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { ":protocol", "websocket" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { ":status", "200" } });
        expectInvalid({ { ":status", "20" } }, { false, false });
        expectInvalid({ { ":method", "CONNECT" }, { ":authority", "example.test" },
            { ":scheme", "https" } });
        expectInvalid({ { ":method", "CONNECT" }, { ":authority", "example.test" },
            { ":protocol", "websocket" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { "x-test", "bad\nvalue" } });
        expectInvalid({ { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/" },
            { "x-test", " trailing" } });
        expectInvalid({ { ":method", "GET" } }, { true, true });

        const std::vector<HttpHeader> connect{
            { ":method", "CONNECT" }, { ":authority", "example.test" }
        };
        Require(LikesProgram::Http::ValidateHttp2HeaderBlock(connect).IsOk(),
            "ordinary CONNECT header block should validate");
        const std::vector<HttpHeader> extendedConnect{
            { ":method", "CONNECT" }, { ":protocol", "websocket" },
            { ":scheme", "https" }, { ":authority", "example.test" }, { ":path", "/chat" }
        };
        Require(LikesProgram::Http::ValidateHttp3HeaderBlock(extendedConnect).IsOk(),
            "extended CONNECT header block should validate");
    }

    void TestHttp1ConnectionLifecycle() {
        using namespace LikesProgram::Http;

        Require(ClassifyHttp1RequestTarget("GET", "/resource").Value()
                == Http1RequestTargetForm::Origin,
            "HTTP/1 target classifier should recognize origin-form");
        Require(ClassifyHttp1RequestTarget("GET", "http://proxy.test/resource").Value()
                == Http1RequestTargetForm::Absolute,
            "HTTP/1 target classifier should recognize absolute-form");
        Require(ClassifyHttp1RequestTarget("CONNECT", "proxy.test:443").Value()
                == Http1RequestTargetForm::Authority,
            "HTTP/1 target classifier should recognize CONNECT authority-form");
        Require(ClassifyHttp1RequestTarget("OPTIONS", "*").Value()
                == Http1RequestTargetForm::Asterisk,
            "HTTP/1 target classifier should recognize asterisk-form");
        Require(!ClassifyHttp1RequestTarget("CONNECT", "/wrong").IsOk(),
            "CONNECT target classifier should reject origin-form");

        Http1Connection requests(Http1MessageKind::Request);
        const auto initialRequestSnapshot = requests.Snapshot();
        Require(initialRequestSnapshot.kind == Http1MessageKind::Request
                && initialRequestSnapshot.state == Http1ConnectionState::Open
                && initialRequestSnapshot.bufferedBytes == 0
                && !initialRequestSnapshot.finished,
            "HTTP/1 snapshot should expose connection kind, state, limits, and buffered bytes");
        Require(requests.Feed(
            "GET /one HTTP/1.1\r\nContent-Length: 0\r\n\r\n"
            "GET /two HTTP/1.1\r\n\r\n").IsOk(),
            "HTTP/1 request reader should accept pipelined bytes");
        const auto firstRequest = requests.NextRequest();
        Require(firstRequest.IsOk() && firstRequest.Value().has_value()
                && firstRequest.Value()->target == "/one"
                && requests.BufferedBytes() != 0,
            "HTTP/1 request reader should preserve residual pipeline bytes");
        const auto secondRequest = requests.NextRequest();
        Require(secondRequest.IsOk() && secondRequest.Value().has_value()
                && secondRequest.Value()->target == "/two"
                && requests.BufferedBytes() == 0,
            "HTTP/1 request reader should consume the second pipeline message");
        const auto emptyRequest = requests.NextRequest();
        Require(emptyRequest.IsOk() && !emptyRequest.Value().has_value(),
            "HTTP/1 request reader should report an empty pipeline");

        Http1Connection closingRequest(Http1MessageKind::Request);
        Require(closingRequest.Feed(
            "GET /close HTTP/1.1\r\nConnection: close\r\n\r\n"
            "GET /late HTTP/1.1\r\n\r\n").IsOk(),
            "HTTP/1 close request should accept bytes for drain inspection");
        Require(closingRequest.NextRequest().IsOk()
                && closingRequest.State() == Http1ConnectionState::Closing,
            "HTTP/1 Connection: close should enter Closing state");
        Require(!closingRequest.NextRequest().IsOk(),
            "HTTP/1 reader should not process a pipelined message after close");
        Require(closingRequest.TakeBufferedBytes().find("/late") != std::string::npos,
            "HTTP/1 reader should expose close residual bytes to the owner");
        Require(closingRequest.Drain().IsOk()
                && closingRequest.State() == Http1ConnectionState::Closed,
            "HTTP/1 reader should close cleanly after draining residual bytes");

        Http1Connection boundedRequest(Http1MessageKind::Request,
            Http1MessageLimits{ 4, 4, 4, 4 });
        RequireStatus(boundedRequest.Feed("123456789"),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/1 connection should enforce its aggregate buffered-byte limit");
        const auto boundedContext = boundedRequest.LastHttpErrorContext();
        Require(boundedContext.valid
                && boundedContext.version == HttpVersion::Http1
                && boundedContext.scope == HttpErrorScope::Connection
                && boundedContext.origin == HttpErrorOrigin::Resource
                && boundedContext.unitKind == HttpErrorUnitKind::None
                && boundedContext.byteOffset == 0
                && boundedContext.statusCode == LikesProgram::StatusCode::ResourceExhausted
                && boundedRequest.LastError().Code()
                    == LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/1 resource failures should retain version and connection context");
        boundedRequest.Reset();
        Require(!boundedRequest.LastHttpErrorContext().valid
                && boundedRequest.LastError().IsOk(),
            "HTTP/1 reset should clear retained diagnostic context");

        Http1Connection responses(Http1MessageKind::Response);
        Require(responses.Feed(
            "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"
            "HTTP/1.1 204 No Content\r\n\r\n").IsOk(),
            "HTTP/1 response reader should accept pipelined responses");
        const auto firstResponse = responses.NextResponse();
        Require(firstResponse.IsOk() && firstResponse.Value().has_value()
                && firstResponse.Value()->body == std::vector<std::uint8_t>{ 'a', 'b', 'c' },
            "HTTP/1 response reader should parse the first framed response");
        const auto secondResponse = responses.NextResponse();
        Require(secondResponse.IsOk() && secondResponse.Value().has_value()
                && secondResponse.Value()->statusCode == 204,
            "HTTP/1 response reader should parse the residual response");

        Http1Connection chunkedResponses(Http1MessageKind::Response);
        Require(chunkedResponses.Feed(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "3\r\nabc\r\n0\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n").IsOk(),
            "HTTP/1 response reader should accept chunked pipeline framing");
        const auto chunkedFirst = chunkedResponses.NextResponse();
        Require(chunkedFirst.IsOk() && chunkedFirst.Value().has_value(),
            "HTTP/1 response reader should isolate a chunked message boundary");
        const auto chunkedResidual = chunkedResponses.NextResponse();
        Require(chunkedResidual.IsOk() && chunkedResidual.Value().has_value()
                && chunkedResidual.Value()->statusCode == 200,
            "HTTP/1 response reader should parse after a chunked message");

        Http1Connection closeDelimited(Http1MessageKind::Response);
        Require(closeDelimited.Feed("HTTP/1.0 200 OK\r\n\r\nlegacy").IsOk(),
            "HTTP/1 response reader should buffer close-delimited responses");
        const auto closeDelimitedPending = closeDelimited.NextResponse();
        Require(closeDelimitedPending.IsOk() && !closeDelimitedPending.Value().has_value(),
            "HTTP/1 response reader should wait for EOF on close-delimited body");
        Require(closeDelimited.Finish().IsOk(),
            "HTTP/1 response reader should accept an EOF boundary");
        const auto legacy = closeDelimited.NextResponse();
        Require(legacy.IsOk() && legacy.Value().has_value()
                && legacy.Value()->body == std::vector<std::uint8_t>{ 'l', 'e', 'g', 'a', 'c', 'y' }
                && closeDelimited.State() == Http1ConnectionState::Closed,
            "HTTP/1 response reader should finish a close-delimited response");

        Http1Connection upgraded(Http1MessageKind::Response);
        Require(upgraded.Feed(
            "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
            "Upgrade: websocket\r\n\r\nraw").IsOk(),
            "HTTP/1 response reader should accept Upgrade handshake bytes");
        const auto upgrade = upgraded.NextResponse(Http1ResponseContext{ false, false, true });
        Require(upgrade.IsOk() && upgrade.Value().has_value()
                && upgraded.State() == Http1ConnectionState::Upgraded
                && upgraded.TakeBufferedBytes() == "raw",
            "HTTP/1 Upgrade should transfer residual bytes to the adapter");

        Http1Connection tunnel(Http1MessageKind::Response);
        Require(tunnel.Feed("HTTP/1.1 200 Connection Established\r\n\r\nraw").IsOk(),
            "HTTP/1 CONNECT response reader should accept tunnel bytes");
        const auto tunnelResponse = tunnel.NextResponse(Http1ResponseContext{ false, true });
        Require(tunnelResponse.IsOk() && tunnelResponse.Value().has_value()
                && tunnel.State() == Http1ConnectionState::Tunnel
                && tunnel.TakeBufferedBytes() == "raw",
            "HTTP/1 CONNECT should transfer residual bytes to the tunnel adapter");

        HttpRequest request;
        request.method = "GET";
        request.target = "/";
        HttpResponse response;
        response.headers = { { "Content-Length", "0" } };
        auto decision = EvaluateHttp1Connection(request, response);
        Require(decision.IsOk() && decision.Value().keepAlive,
            "HTTP/1 connection policy should keep HTTP/1.1 framed messages alive");
        request.version = "HTTP/1.0";
        decision = EvaluateHttp1Connection(request, response);
        Require(decision.IsOk() && !decision.Value().keepAlive,
            "HTTP/1.0 connection policy should close without keep-alive");
        request.version = "HTTP/1.1";
        request.headers = { { "Connection", "Upgrade" }, { "Upgrade", "websocket" } };
        response.statusCode = 101;
        response.reason = "Switching Protocols";
        response.headers = { { "Connection", "Upgrade" }, { "Upgrade", "websocket" } };
        decision = EvaluateHttp1Connection(request, response);
        Require(decision.IsOk() && decision.Value().upgrade && !decision.Value().keepAlive,
            "HTTP/1 connection policy should classify a valid Upgrade handshake");
    }

    void TestHttpSessionCompositionAndFailures() {
        LikesProgram::Http::HttpRequest request; // Session 测试请求
        request.method = "GET";
        request.target = "/session";

        LikesProgram::Http::HttpSession emptySession;
        RequireStatus(emptySession.Send(request), LikesProgram::StatusCode::FailedPrecondition,
            "HttpSession should require an explicit transport");
        RequireStatus(emptySession.Handle(request), LikesProgram::StatusCode::FailedPrecondition,
            "HttpSession should require an explicit handler");
        emptySession.SetDeadline(
            LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now()));
        Require(emptySession.HasDeadline() && emptySession.DeadlineExpired(),
            "empty HttpSession should expose an expired caller deadline");
        emptySession.Cancel();
        Require(emptySession.IsCancelled(),
            "empty HttpSession should retain caller cancellation state");
        emptySession.ResetCancellation();
        Require(!emptySession.IsCancelled(),
            "HttpSession cancellation reset should clear caller state");

        LoopbackTransport transport;
        EchoHandler handler;
        LikesProgram::Http::HttpSession session(&transport, &handler);
        session.SetVersion(LikesProgram::Http::HttpVersion::Http3);
        Require(session.NegotiatedProtocol().version == LikesProgram::Http::HttpVersion::Http3
            && !session.NegotiatedProtocol().datagram
            && std::string_view(session.NegotiatedProtocol().alpn).empty(),
            "SetVersion(Http3) should not infer QUIC or ALPN support");
        const auto sent = session.Send(request);
        Require(sent.IsOk() && sent.Value().statusCode == 204,
            "HttpSession should delegate to user transport");
        Require(transport.lastVersion == LikesProgram::Http::HttpVersion::Http3
            && transport.lastTarget == "/session",
            "HttpSession should preserve request and version");

        LikesProgram::Http::HttpNegotiatedProtocol negotiated{
            LikesProgram::Http::HttpVersion::Http2,
            "h2",
            true,
            false,
            false
        }; // 外部 TCP/TLS ALPN 结果
        session.SetNegotiatedProtocol(negotiated);
        Require(session.Send(request).IsOk(),
            "HttpSession should accept an externally negotiated context");
        Require(transport.lastNegotiated.version == LikesProgram::Http::HttpVersion::Http2
            && std::string_view(transport.lastNegotiated.alpn) == "h2"
            && transport.lastNegotiated.secure
            && !transport.lastNegotiated.datagram,
            "HttpSession should pass ALPN/TLS context to transport");

        LikesProgram::Http::HttpVersion mapped = LikesProgram::Http::HttpVersion::Http1; // ALPN 映射输出
        Require(LikesProgram::Http::TryMapHttpAlpn("h2", mapped)
            && mapped == LikesProgram::Http::HttpVersion::Http2,
            "ALPN h2 should map to HTTP/2");
        Require(LikesProgram::Http::TryMapHttpAlpn("h3-29", mapped)
            && mapped == LikesProgram::Http::HttpVersion::Http3,
            "versioned h3 ALPN should map to HTTP/3");
        Require(!LikesProgram::Http::TryMapHttpAlpn("unknown", mapped),
            "unknown ALPN must not guess a protocol");

        session.SetDeadline(
            LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now()));
        Require(session.HasDeadline() && session.DeadlineExpired(),
            "HttpSession should expose an expired caller deadline");
        RequireStatus(session.Send(request), LikesProgram::StatusCode::DeadlineExceeded,
            "HttpSession should gate transport sends on an expired deadline");
        RequireStatus(session.Handle(request), LikesProgram::StatusCode::DeadlineExceeded,
            "HttpSession should gate handler calls on an expired deadline");
        session.SetDeadline(LikesProgram::Time::Deadline::Infinite());
        Require(!session.HasDeadline() && !session.DeadlineExpired(),
            "HttpSession infinite deadline should disable the deadline gate");
        session.Cancel();
        RequireStatus(session.Send(request), LikesProgram::StatusCode::Cancelled,
            "HttpSession should gate transport sends on caller cancellation");
        RequireStatus(session.Handle(request), LikesProgram::StatusCode::Cancelled,
            "HttpSession should gate handler calls on caller cancellation");
        session.ResetCancellation();
        Require(!session.IsCancelled() && session.Send(request).IsOk(),
            "HttpSession cancellation reset should restore request delivery");

        Require(session.SetNegotiatedAlpn("h3", true, true),
            "HTTP/3 ALPN should require and accept secure datagram context");
        const auto h3Negotiated = session.NegotiatedProtocol();
        Require(h3Negotiated.version == LikesProgram::Http::HttpVersion::Http3
            && h3Negotiated.secure
            && h3Negotiated.datagram
            && !h3Negotiated.fallback,
            "HTTP/3 ALPN should populate the negotiated context");
        Require(!session.SetNegotiatedAlpn("h3", false, true),
            "HTTP/3 ALPN should reject an insecure context");
        Require(!session.SetNegotiatedAlpn("h3", true, false),
            "HTTP/3 ALPN should reject a non-datagram context");
        Require(!session.SetNegotiatedAlpn("h2", true, true),
            "HTTP/2 ALPN should reject a datagram context");
        Require(!session.SetNegotiatedAlpn("unknown", true, false),
            "unknown ALPN should leave the negotiated context unchanged");
        Require(session.NegotiatedProtocol().version == LikesProgram::Http::HttpVersion::Http3
            && std::string_view(session.NegotiatedProtocol().alpn) == "h3",
            "rejected ALPN context must not replace the previous context");

        const auto handled = session.Handle(request);
        Require(handled.IsOk()
            && handled.Value().body == std::vector<std::uint8_t>{ '/', 's', 'e', 's', 's', 'i', 'o', 'n' },
            "HttpSession should delegate to user handler");

        session.SetTransport(nullptr);
        session.SetHandler(nullptr);
        RequireStatus(session.Send(request), LikesProgram::StatusCode::FailedPrecondition,
            "HttpSession should support transport unbinding");
        RequireStatus(session.Handle(request), LikesProgram::StatusCode::FailedPrecondition,
            "HttpSession should support handler unbinding");

        FailingTransport failingTransport;
        session.SetTransport(&failingTransport);
        RequireStatus(session.Send(request), LikesProgram::StatusCode::Unavailable,
            "HttpSession should preserve transport failure status");

        ThrowingTransport throwingTransport;
        session.SetTransport(&throwingTransport);
        RequireStatus(session.Send(request), LikesProgram::StatusCode::Internal,
            "HttpSession should isolate transport exceptions");

        ThrowingHandler throwingHandler;
        session.SetHandler(&throwingHandler);
        RequireStatus(session.Handle(request), LikesProgram::StatusCode::Internal,
            "HttpSession should isolate handler exceptions");

        Require(std::string(LikesProgram::Http::HttpVersionName(LikesProgram::Http::HttpVersion::Http1))
            == "HTTP/1.1", "HTTP/1 version name mismatch");
        Require(std::string(LikesProgram::Http::HttpVersionName(LikesProgram::Http::HttpVersion::Http2))
            == "HTTP/2", "HTTP/2 version name mismatch");
        Require(std::string(LikesProgram::Http::HttpVersionName(LikesProgram::Http::HttpVersion::Http3))
            == "HTTP/3", "HTTP/3 version name mismatch");
        Require(std::string(LikesProgram::Http::HttpVersionName(
            static_cast<LikesProgram::Http::HttpVersion>(0xFF))) == "UNKNOWN",
            "Unknown HTTP version name mismatch");
    }

    void TestHttpSessionRepeatedLifecycle() {
        LoopbackTransport transport;                          // 跨 Session 复用的用户适配器
        EchoHandler handler;                                  // 跨 Session 复用的用户处理器
        LikesProgram::Http::HttpRequest request;
        request.method = "GET";
        request.target = "/repeat";

        for (int iteration = 0; iteration < 5000; ++iteration) {
            LikesProgram::Http::HttpSession session(&transport, &handler);
            session.SetVersion(static_cast<LikesProgram::Http::HttpVersion>(iteration % 3));
            Require(session.Send(request).IsOk(), "HttpSession repeated Send should succeed");
            Require(session.Handle(request).IsOk(), "HttpSession repeated Handle should succeed");
        }
    }

    void TestHttpAltSvcCache() {
        using namespace LikesProgram::Http;

        HttpAltSvcCache cache({ 2, 64, 256 });
        Require(cache.Observe("https://example.test", "h3=\":443\"; ma=60", 100).IsOk(),
            "Alt-Svc cache should accept a bounded HTTP/3 advertisement");
        const auto entry = cache.Lookup("https://example.test", 100);
        Require(entry.IsOk() && entry.Value().has_value()
            && entry.Value()->alpn == "h3"
            && entry.Value()->authority == ":443"
            && entry.Value()->expiresAt == 160,
            "Alt-Svc lookup should expose the parsed authority and expiry");

        const auto h3 = cache.Select("https://example.test", 120);
        Require(h3.IsOk() && h3.Value().version == HttpVersion::Http3
            && !h3.Value().fallback
            && h3.Value().alpn == "h3"
            && h3.Value().authority == ":443",
            "Alt-Svc selection should choose HTTP/3 while the entry is live");
        HttpSession session;
        Require(TryApplyHttpAltSvcSelection(session, h3.Value(), true, true),
            "Alt-Svc H3 selection should apply to a Session");
        Require(session.NegotiatedProtocol().version == HttpVersion::Http3
            && session.NegotiatedProtocol().datagram,
            "applied Alt-Svc H3 selection should preserve secure datagram context");
        Require(!TryApplyHttpAltSvcSelection(session, h3.Value(), true, false),
            "Alt-Svc H3 selection should reject a non-datagram transport");
        Require(session.NegotiatedProtocol().version == HttpVersion::Http3,
            "rejected Alt-Svc application should preserve the prior Session context");
        const auto tcp = cache.Select("https://example.test", 120, HttpVersion::Http2, false);
        Require(tcp.IsOk() && tcp.Value().version == HttpVersion::Http2
            && tcp.Value().fallback,
            "Alt-Svc selection should preserve TCP fallback when QUIC is unavailable");
        Require(TryApplyHttpAltSvcSelection(session, tcp.Value(), true, false),
            "Alt-Svc TCP fallback should apply to a Session");
        Require(session.NegotiatedProtocol().version == HttpVersion::Http2
            && session.NegotiatedProtocol().fallback
            && !session.NegotiatedProtocol().datagram,
            "applied TCP fallback should expose its negotiated context");

        Require(!cache.Observe("https://example.test", "h3=:443", 120).IsOk(),
            "Alt-Svc cache should reject an unquoted authority");
        const auto preserved = cache.Lookup("https://example.test", 120);
        Require(preserved.IsOk() && preserved.Value().has_value()
            && preserved.Value()->expiresAt == 160,
            "malformed Alt-Svc must not replace a prior entry");
        const auto expired = cache.Lookup("https://example.test", 160);
        Require(expired.IsOk() && !expired.Value().has_value(),
            "Alt-Svc entry should expire at its caller-supplied deadline");

        Require(cache.Observe("https://example.test", "clear", 160).IsOk(),
            "Alt-Svc clear token should remove the origin entry");
        Require(cache.Snapshot(160).entries == 0,
            "Alt-Svc clear should remove expired and live origin state");
        Require(!cache.Select("https://example.test", 160, HttpVersion::Http3).IsOk(),
            "HTTP/3 cannot be selected as a TCP fallback version");

        Require(cache.Observe("https://one.test", "h3=\":443\"; ma=0", 10).IsOk()
            && cache.Observe("https://two.test", "h3-29=\"alt.test:8443\"", 10).IsOk(),
            "Alt-Svc cache should retain bounded entries and versioned H3 tokens");
        Require(!cache.Observe("https://three.test", "h3=\":443\"", 10).IsOk(),
            "Alt-Svc cache should enforce its entry limit");
        Require(cache.Snapshot(10).expiredEntries == 1
            && cache.Snapshot(10).liveEntries == 1,
            "Alt-Svc snapshot should distinguish live and expired entries");
        cache.Reset();
        Require(cache.Snapshot(10).entries == 0,
            "Alt-Svc Reset should clear all origin state");
    }

    void TestHttpSessionProtocolFallbackReplay() {
        using namespace LikesProgram::Http;

        HttpRequest get;
        get.method = "GET";
        get.target = "/replay?id=42";
        ReplayTransport transport;
        HttpSession session(&transport);
        Require(session.SetNegotiatedAlpn("h3", true, true),
            "fallback replay fixture should start in secure HTTP/3");
        const auto replayed = session.SendWithFallback(get, HttpVersion::Http2);
        Require(replayed.IsOk() && replayed.Value().statusCode == 200
                && std::string(replayed.Value().body.begin(), replayed.Value().body.end())
                    == "replay",
            "idempotent request should replay after an unavailable H3 attempt");
        Require(transport.versions == std::vector<HttpVersion>{
                    HttpVersion::Http3, HttpVersion::Http2 }
                && transport.targets == std::vector<std::string>{
                    "/replay?id=42", "/replay?id=42" },
            "fallback replay should preserve the request and switch only the protocol");
        const auto negotiated = session.NegotiatedProtocol();
        Require(negotiated.version == HttpVersion::Http2
                && std::string_view(negotiated.alpn) == "h2"
                && negotiated.secure && !negotiated.datagram
                && negotiated.fallback,
            "fallback replay should expose the explicit secure TCP context");

        HttpRequest post;
        post.method = "POST";
        post.target = "/orders";
        post.body = { 'x' };
        ReplayTransport unsafeTransport;
        HttpSession unsafeSession(&unsafeTransport);
        Require(unsafeSession.SetNegotiatedAlpn("h3", true, true),
            "unsafe replay fixture should start in secure HTTP/3");
        const auto unsafe = unsafeSession.SendWithFallback(post, HttpVersion::Http1);
        RequireStatus(unsafe, LikesProgram::StatusCode::Unavailable,
            "non-idempotent request should preserve the first transport failure");
        Require(unsafeTransport.versions == std::vector<HttpVersion>{ HttpVersion::Http3 }
                && unsafeSession.Version() == HttpVersion::Http3,
            "non-idempotent request must not switch protocol without explicit replay consent");

        HttpRequest idempotencyKeyPost = post;
        idempotencyKeyPost.headers.push_back({ "Idempotency-Key", "order-42" });
        HttpRequestReplayPolicy optedIn{};
        optedIn.allowUnsafeMethodsWithIdempotencyKey = true;
        ReplayTransport optedInTransport;
        HttpSession optedInSession(&optedInTransport);
        Require(optedInSession.SetNegotiatedAlpn("h3", true, true)
                && optedInSession.SendWithFallback(
                    idempotencyKeyPost, HttpVersion::Http1, optedIn).IsOk()
                && optedInTransport.versions == std::vector<HttpVersion>{
                    HttpVersion::Http3, HttpVersion::Http1 },
            "explicit Idempotency-Key policy should permit one unsafe-method replay");

        HttpRequestReplayPolicy oneAttempt{};
        oneAttempt.maxAttempts = 1;
        ReplayTransport oneAttemptTransport;
        HttpSession oneAttemptSession(&oneAttemptTransport);
        Require(oneAttemptSession.SetNegotiatedAlpn("h3", true, true),
            "one-attempt fixture should start in secure HTTP/3");
        RequireStatus(oneAttemptSession.SendWithFallback(
                get, HttpVersion::Http2, oneAttempt),
            LikesProgram::StatusCode::Unavailable,
            "one-attempt policy should preserve the first transport failure");
        Require(oneAttemptTransport.versions.size() == 1,
            "one-attempt policy must not replay");

        HttpRequestReplayPolicy invalid{};
        invalid.maxAttempts = 3;
        RequireStatus(oneAttemptSession.SendWithFallback(
                get, HttpVersion::Http2, invalid),
            LikesProgram::StatusCode::OutOfRange,
            "replay policy should reject more than one fallback attempt");
        RequireStatus(oneAttemptSession.SendWithFallback(get, HttpVersion::Http3),
            LikesProgram::StatusCode::InvalidArgument,
            "fallback replay should reject HTTP/3 as a TCP fallback");
    }

    void TestHttpConnectionPoolLifecycle() {
        using namespace LikesProgram;
        using namespace LikesProgram::Http;

        HttpConnectionPoolLimits limits;
        limits.maxConnections = 3;
        limits.maxConnectionsPerOrigin = 2;
        limits.maxConcurrentStreamsPerConnection = 4;
        HttpConnectionPool pool(limits);

        HttpConnectionDescriptor h2;
        h2.origin = "https://example.test";
        h2.version = HttpVersion::Http2;
        h2.secure = true;
        h2.maxConcurrentStreams = 2;
        const auto h2Id = pool.AddConnection(h2);
        Require(h2Id.IsOk(), "connection pool should register an H2 slot");

        HttpConnectionDescriptor h3;
        h3.origin = h2.origin;
        h3.version = HttpVersion::Http3;
        h3.secure = true;
        h3.datagram = true;
        h3.maxConcurrentStreams = 2;
        const auto h3Id = pool.AddConnection(h3);
        Require(h3Id.IsOk(), "connection pool should register an H3 slot");

        HttpConnectionDescriptor invalidH3 = h3;
        invalidH3.datagram = false;
        RequireStatus(pool.AddConnection(invalidH3), StatusCode::InvalidArgument,
            "H3 pool entries must carry an external datagram path");
        HttpConnectionDescriptor invalidH1;
        invalidH1.origin = h2.origin;
        invalidH1.maxConcurrentStreams = 2;
        RequireStatus(pool.AddConnection(invalidH1), StatusCode::InvalidArgument,
            "H1 pool entries must have one in-flight stream");

        const auto first = pool.Acquire(h2.origin, HttpVersion::Http2, true, false);
        const auto second = pool.Acquire(h2.origin, HttpVersion::Http2, true, false);
        Require(first.IsOk() && second.IsOk()
                && first.Value().connectionId == h2Id.Value()
                && second.Value().connectionId == h2Id.Value()
                && first.Value().streamId == 1
                && second.Value().streamId == 3,
            "H2 pool leases should use one connection and monotonic client streams");
        RequireStatus(pool.Acquire(h2.origin, HttpVersion::Http2, true, false),
            StatusCode::ResourceExhausted,
            "H2 pool should enforce the registered stream capacity");
        Require(pool.Release(first.Value()).IsOk(),
            "connection pool should release an active H2 lease");
        const auto third = pool.Acquire(h2.origin, HttpVersion::Http2, true, false);
        Require(third.IsOk() && third.Value().streamId == 5,
            "H2 pool should allocate a fresh stream id after release");

        Require(pool.MarkDraining(h2Id.Value(), 3).IsOk(),
            "H2 pool should accept a peer GOAWAY drain boundary");
        RequireStatus(pool.Acquire(h2.origin, HttpVersion::Http2, true, false),
            StatusCode::FailedPrecondition,
            "draining H2 connections must reject new leases");
        RequireStatus(pool.MarkDraining(h2Id.Value(), 5), StatusCode::OutOfRange,
            "H2 GOAWAY boundaries must not increase");
        Require(pool.Release(second.Value()).IsOk() && pool.Release(third.Value()).IsOk(),
            "H2 leases opened before GOAWAY should remain releasable");

        const auto h3Lease = pool.Acquire(h3.origin, HttpVersion::Http3, true, true);
        Require(h3Lease.IsOk() && h3Lease.Value().pathGeneration == 0,
            "H3 pool should issue a lease on the ready path");
        Require(pool.BeginMigration(h3Id.Value(), 42).IsOk(),
            "H3 pool should begin a caller-owned path migration");
        RequireStatus(pool.Acquire(h3.origin, HttpVersion::Http3, true, true),
            StatusCode::FailedPrecondition,
            "H3 migration must quiesce new stream leases");
        RequireStatus(pool.CommitMigration(h3Id.Value(), 41), StatusCode::FailedPrecondition,
            "H3 migration must reject an unrelated path token");
        Require(pool.CommitMigration(h3Id.Value(), 42).IsOk(),
            "H3 migration should commit after external path validation");
        const auto migratedLease = pool.Acquire(h3.origin, HttpVersion::Http3, true, true);
        Require(migratedLease.IsOk() && migratedLease.Value().pathGeneration == 1,
            "H3 leases should expose the committed path generation");
        Require(h3Lease.Value().streamId == 0 && migratedLease.Value().streamId == 4,
            "H3 client bidirectional stream ids should advance by four");
        Require(pool.Release(h3Lease.Value()).IsOk()
                && pool.Release(migratedLease.Value()).IsOk(),
            "H3 leases should survive a committed path migration");

        Require(pool.BeginMigration(h3Id.Value(), 43).IsOk()
                && pool.AbortMigration(h3Id.Value(), 43).IsOk(),
            "H3 migration abort should be explicit and terminal for new leases");
        RequireStatus(pool.Acquire(h3.origin, HttpVersion::Http3, true, true),
            StatusCode::FailedPrecondition,
            "an aborted H3 migration should drain the affected connection");

        const auto snapshot = pool.Snapshot();
        Require(snapshot.connections == 2
                && snapshot.drainingConnections == 2
                && snapshot.activeLeases == 0,
            "connection pool snapshot should expose drain and lease accounting");
        Require(pool.Close(h2Id.Value()).IsOk(),
            "connection pool should close a drained connection");
        RequireStatus(pool.Close(h2Id.Value()), StatusCode::NotFound,
            "connection pool close should reject a repeated connection id");

        HttpConnectionDescriptor resetH1;
        resetH1.origin = "http://reset.test";
        const auto oldId = pool.AddConnection(resetH1);
        const auto oldLease = pool.Acquire(
            resetH1.origin, HttpVersion::Http1, false, false);
        Require(oldId.IsOk() && oldLease.IsOk(),
            "connection pool reset fixture should acquire an H1 lease");
        pool.Reset();
        const auto newId = pool.AddConnection(resetH1);
        const auto newLease = pool.Acquire(
            resetH1.origin, HttpVersion::Http1, false, false);
        Require(newId.IsOk() && newLease.IsOk() && newId.Value() != oldId.Value(),
            "connection pool reset must not reuse an invalidated connection id");
        RequireStatus(pool.Release(oldLease.Value()), StatusCode::NotFound,
            "an invalidated lease must not release a post-reset connection");
        Require(pool.Release(newLease.Value()).IsOk(),
            "a post-reset connection lease should remain valid");

        const auto reusedH1 = pool.Acquire(
            resetH1.origin, HttpVersion::Http1, false, false);
        Require(reusedH1.IsOk() && reusedH1.Value().streamId == newLease.Value().streamId
                && reusedH1.Value().leaseId != newLease.Value().leaseId,
            "H1 reuse should issue a unique lease id despite its fixed stream id");
        RequireStatus(pool.Release(newLease.Value()), StatusCode::FailedPrecondition,
            "an old H1 lease must not release a newer request on the same connection");
        Require(pool.Release(reusedH1.Value()).IsOk(),
            "the current H1 lease should remain releasable after stale-release rejection");

        HttpConnectionDescriptor secondOriginSlot = resetH1;
        Require(pool.AddConnection(secondOriginSlot).IsOk(),
            "connection pool should add a second slot for limit-shrink validation");
        auto shrunken = limits;
        shrunken.maxConnectionsPerOrigin = 1;
        RequireStatus(pool.SetLimits(shrunken), StatusCode::FailedPrecondition,
            "connection pool must reject limits below an existing origin count");
    }

    void TestHttpWebsiteSemantics() {
        using namespace LikesProgram;
        using namespace LikesProgram::Http;

        const auto uri = ParseHttpUri("https://Example.COM:443/a/../docs/?q=1#fragment");
        Require(uri.IsOk()
                && uri.Value().scheme == "https"
                && uri.Value().host == "example.com"
                && uri.Value().authority == "example.com:443"
                && uri.Value().pathAndQuery == "/docs/?q=1"
                && HttpUriOrigin(uri.Value()) == "https://example.com",
            "HTTP URI parsing should normalize origin, path and fragment semantics");
        const auto relative = ResolveHttpUri(
            "https://example.com/a/b/index.html?old=1", "../asset.js?v=2#ignored");
        Require(relative.IsOk()
                && relative.Value() == "https://example.com/a/asset.js?v=2",
            "HTTP URI resolution should apply relative path and query semantics");
        const auto nestedAbsoluteQuery = ResolveHttpUri(
            "https://example.com/start", "/login?next=https://other.test/path");
        Require(nestedAbsoluteQuery.IsOk()
                && nestedAbsoluteQuery.Value()
                    == "https://example.com/login?next=https://other.test/path",
            "an absolute URI inside a relative Location query must not replace its origin");
        RequireStatus(ParseHttpUri("https://user@example.com/private"),
            StatusCode::InvalidArgument,
            "HTTP URI parsing should reject userinfo at the website boundary");

        HttpRequest post;
        post.method = "POST";
        post.target = "/submit";
        post.headers = {
            { "Host", "example.com" },
            { "Authorization", "Bearer secret" },
            { "Cookie", "sid=secret" },
            { "Content-Type", "application/json" },
            { "Content-Length", "2" }
        };
        post.body = { '{', '}' };
        HttpResponse redirect;
        redirect.statusCode = 302;
        redirect.headers = { { "Location", "https://other.test/final?q=1" } };
        const auto redirected = PrepareHttpRedirect(
            "https://example.com/submit", post, redirect, 0);
        Require(redirected.IsOk()
                && redirected.Value().follow
                && redirected.Value().originChanged
                && !redirected.Value().bodyPreserved
                && redirected.Value().request.method == "GET"
                && redirected.Value().request.target == "/final?q=1"
                && redirected.Value().request.body.empty()
                && HttpHeaderValue(redirected.Value().request.headers, "host")
                    == "other.test"
                && HttpHeaderValue(redirected.Value().request.headers, "authorization").empty()
                && HttpHeaderValue(redirected.Value().request.headers, "cookie").empty()
                && HttpHeaderValue(redirected.Value().request.headers, "content-type").empty(),
            "cross-origin 302 should rewrite POST and strip credentials and entity metadata");
        redirect.statusCode = 307;
        redirect.headers = { { "Location", "/retry" } };
        const auto preserved = PrepareHttpRedirect(
            "https://example.com/submit", post, redirect, 1);
        Require(preserved.IsOk()
                && preserved.Value().request.method == "POST"
                && preserved.Value().request.body == post.body
                && HttpHeaderValue(preserved.Value().request.headers, "authorization")
                    == "Bearer secret",
            "same-origin 307 should preserve method, body and credentials");
        redirect.statusCode = 301;
        redirect.headers = { { "Location", "http://example.com/plain" } };
        RequireStatus(PrepareHttpRedirect(
                "https://example.com/start", post, redirect, 0),
            StatusCode::PermissionDenied,
            "redirect policy should reject HTTPS downgrade by default");

        struct DomainPolicy final : HttpCookieDomainPolicy {
            bool AcceptDomain(
                std::string_view,
                std::string_view domain) const noexcept override {
                return domain == "example.com";
            }
        } domainPolicy;
        HttpCookieJar jar({}, &domainPolicy);
        Require(jar.Store(
                "https://app.example.com/account/login",
                "sid=host; Path=/account; Secure; HttpOnly; SameSite=Lax",
                1000).IsOk(),
            "cookie jar should store a secure host-only cookie");
        Require(jar.Store(
                "https://app.example.com/account/login",
                "quoted=\"hello-world\"; Path=/account; Max-Age=120",
                1000).IsOk(),
            "cookie jar should accept an RFC quoted cookie value");
        Require(jar.Store(
                "https://app.example.com/account/login",
                "theme=dark; Domain=example.com; Path=/; Max-Age=60; SameSite=None; Secure",
                1000).IsOk(),
            "cookie jar should store an accepted scoped domain cookie");
        const auto accountCookies = jar.CookieHeader(
            "https://app.example.com/account/view", 1010);
        Require(accountCookies.IsOk()
                && accountCookies.Value() == "sid=host; quoted=hello-world; theme=dark",
            "cookie jar should order matching cookies by longest path");
        const auto siblingCookies = jar.CookieHeader(
            "https://cdn.example.com/assets", 1010);
        Require(siblingCookies.IsOk() && siblingCookies.Value() == "theme=dark",
            "domain cookie should apply to a matching sibling while host-only cookie does not");
        HttpCookieRequestContext crossSiteSubresource;
        crossSiteSubresource.sameSite = false;
        crossSiteSubresource.topLevelNavigation = false;
        const auto crossSiteCookies = jar.CookieHeader(
            "https://app.example.com/account/view", 1010, crossSiteSubresource);
        Require(crossSiteCookies.IsOk() && crossSiteCookies.Value() == "theme=dark",
            "cross-site subresources should exclude Lax/default cookies while retaining SameSite=None");
        const auto insecureCookies = jar.CookieHeader(
            "http://app.example.com/account/view", 1010);
        Require(insecureCookies.IsOk() && insecureCookies.Value() == "quoted=hello-world",
            "secure cookies must not be emitted over HTTP while non-secure cookies remain usable");
        RequireStatus(jar.Store(
                "http://app.example.com/", "bad=value; Secure", 1010),
            StatusCode::PermissionDenied,
            "an insecure response must not create a Secure cookie");
        Require(jar.Store(
                "https://app.example.com/", "theme=gone; Domain=example.com; Path=/; Max-Age=0",
                1020).IsOk()
                && jar.CookieHeader("https://cdn.example.com/", 1020).Value().empty(),
            "Max-Age zero should delete the matching cookie");
        Require(jar.Store(
                "https://app.example.com/", "expires=soon; Path=/; Expires=Wed, 09 Jun 2030 10:18:14 GMT",
                1000).IsOk()
                && jar.Snapshot(1000).persistentCookies == 2,
            "cookie jar should parse an RFC cookie Expires date");
        Require(jar.Snapshot(1020).cookies == 3
                && jar.Snapshot(1020).secureCookies == 1,
            "cookie snapshot should expose bounded retained state");

        const auto ranges = ParseHttpByteRanges("bytes=0-9, -5, 95-");
        Require(ranges.IsOk() && ranges.Value().size() == 3,
            "Range parser should accept closed, suffix and open-ended byte ranges");
        const auto resolvedRanges = ResolveHttpByteRanges(ranges.Value(), 100);
        Require(resolvedRanges.IsOk()
                && resolvedRanges.Value()[0].first == 0
                && resolvedRanges.Value()[0].last == 9
                && resolvedRanges.Value()[1].first == 95
                && resolvedRanges.Value()[2].last == 99
                && BuildHttpContentRange(resolvedRanges.Value()[0], 100)
                    == "bytes 0-9/100"
                && BuildHttpUnsatisfiedContentRange(100) == "bytes */100",
            "Range resolution should produce valid 206 and 416 Content-Range values");
        const auto unsatisfiedSpecs = ParseHttpByteRanges("bytes=200-300");
        Require(unsatisfiedSpecs.IsOk(),
            "syntactically valid unsatisfied range should parse before length resolution");
        RequireStatus(ResolveHttpByteRanges(unsatisfiedSpecs.Value(), 100),
            StatusCode::NotFound,
            "range resolution should distinguish a valid but unsatisfied range");

        struct RecordingDecoder final : HttpContentDecoder {
            std::vector<std::string> calls;

            Result<std::vector<std::uint8_t>> Decode(
                std::string_view coding,
                std::span<const std::uint8_t> input,
                std::size_t maxOutputBytes) override {
                calls.emplace_back(coding);
                std::vector<std::uint8_t> output(input.begin(), input.end());
                output.push_back(static_cast<std::uint8_t>(coding.front()));
                if (output.size() > maxOutputBytes) {
                    return Status(StatusCode::ResourceExhausted,
                        u"test decoder output limit");
                }
                return output;
            }
        } decoder;
        HttpResponse encoded;
        encoded.statusCode = 200;
        encoded.headers = {
            { "Content-Encoding", "br, gzip" }, { "Content-Length", "1" }
        };
        encoded.body = { 'x' };
        const auto decoded = DecodeHttpContent(encoded, decoder, { 2, 8 });
        Require(decoded.IsOk()
                && decoder.calls == std::vector<std::string>{ "gzip", "br" }
                && decoded.Value().body == std::vector<std::uint8_t>{ 'x', 'g', 'b' }
                && HttpHeaderValue(decoded.Value().headers, "content-encoding").empty()
                && HttpHeaderValue(decoded.Value().headers, "content-length") == "3",
            "content decoding should apply codings in reverse and normalize metadata");

        HttpMultipartPart textPart;
        textPart.headers = {
            { "Content-Disposition", "form-data; name=\"title\"" }
        };
        textPart.body = { 'h', 'e', 'l', 'l', 'o' };
        HttpMultipartPart filePart;
        filePart.headers = {
            { "Content-Disposition", "form-data; name=\"upload\"; filename=\"a.txt\"" },
            { "Content-Type", "text/plain" }
        };
        filePart.body = { 0, 'x', '\r', '\n', 'y' };
        const std::array<HttpMultipartPart, 2> multipartParts{ textPart, filePart };
        const auto multipartWire = BuildHttpMultipart(
            "likes-boundary", multipartParts);
        Require(multipartWire.IsOk(),
            "multipart builder should produce a bounded browser form body");
        const auto parsedMultipart = ParseHttpMultipart(
            "multipart/form-data; charset=utf-8; boundary=\"likes-boundary\"",
            multipartWire.Value());
        Require(parsedMultipart.IsOk()
                && parsedMultipart.Value().size() == 2
                && parsedMultipart.Value()[0].name == "title"
                && parsedMultipart.Value()[0].body == textPart.body
                && parsedMultipart.Value()[1].name == "upload"
                && parsedMultipart.Value()[1].filename == "a.txt"
                && parsedMultipart.Value()[1].body == filePart.body,
            "multipart parser should recover disposition metadata and binary bodies");
        HttpMultipartLimits onePartLimit;
        onePartLimit.maxParts = 1;
        RequireStatus(ParseHttpMultipart(
                "multipart/form-data; boundary=likes-boundary",
                multipartWire.Value(), onePartLimit),
            StatusCode::ResourceExhausted,
            "multipart parser should enforce its part-count limit");
        filePart.headers.push_back({ "X-Unsafe", "line\r\nbreak" });
        const std::array<HttpMultipartPart, 1> unsafePart{ filePart };
        RequireStatus(BuildHttpMultipart("likes-boundary", unsafePart),
            StatusCode::InvalidArgument,
            "multipart builder should reject header injection");

        const auto challenges = ParseHttpAuthenticationChallenges(
            "Digest realm=\"site\", qop=\"auth,auth-int\", Basic realm=\"fallback\"");
        Require(challenges.IsOk()
                && challenges.Value().size() == 2
                && challenges.Value()[0].scheme == "digest"
                && challenges.Value()[0].credentials
                    == "realm=\"site\", qop=\"auth,auth-int\""
                && challenges.Value()[1].scheme == "basic",
            "authentication parser should preserve quoted commas and parameter continuations");
        const auto basic = BuildHttpBasicAuthorization("user", "pass");
        const auto bearer = BuildHttpBearerAuthorization("abc.DEF_123~+/==");
        Require(basic.IsOk() && basic.Value() == "Basic dXNlcjpwYXNz"
                && bearer.IsOk() && bearer.Value() == "Bearer abc.DEF_123~+/==",
            "authentication helpers should build valid Basic and Bearer credentials");
        RequireStatus(BuildHttpBasicAuthorization("bad:user", "pass"),
            StatusCode::InvalidArgument,
            "Basic authentication should reject a colon in user-id");
        RequireStatus(BuildHttpBearerAuthorization("abc=tail"),
            StatusCode::InvalidArgument,
            "Bearer padding must be terminal");

        HttpRequest cacheRequest;
        cacheRequest.method = "GET";
        cacheRequest.target = "/asset";
        cacheRequest.headers = { { "Authorization", "Bearer token" } };
        HttpResponse cacheResponse;
        cacheResponse.statusCode = 200;
        cacheResponse.headers = {
            { "Cache-Control", "public, max-age=60, s-maxage=30, stale-if-error=120" },
            { "ETag", "\"v1\"" }
        };
        const auto cacheControl = ParseHttpCacheControl(cacheResponse.headers);
        const auto cacheEvaluation = EvaluateHttpCacheResponse(
            cacheRequest, cacheResponse, true);
        Require(cacheControl.IsOk()
                && cacheControl.Value().isPublic
                && cacheControl.Value().maxAge == 60
                && cacheControl.Value().sharedMaxAge == 30
                && cacheControl.Value().staleIfError == 120
                && cacheEvaluation.IsOk()
                && cacheEvaluation.Value().cacheable
                && cacheEvaluation.Value().freshnessLifetime == 30,
            "cache helpers should parse freshness and permit explicitly public authorized responses");
        HttpCacheValidators validators;
        validators.etag = "\"v1\"";
        validators.lastModified = "Wed, 09 Jun 2021 10:18:14 GMT";
        Require(ApplyHttpCacheValidators(cacheRequest, validators).IsOk()
                && HttpHeaderValue(cacheRequest.headers, "if-none-match") == "\"v1\""
                && HttpHeaderValue(cacheRequest.headers, "if-modified-since")
                    == "Wed, 09 Jun 2021 10:18:14 GMT",
            "cache validator helper should create a conditional request");
        cacheResponse.headers = { { "Cache-Control", "no-store" } };
        const auto noStore = EvaluateHttpCacheResponse(cacheRequest, cacheResponse, false);
        Require(noStore.IsOk() && !noStore.Value().cacheable,
            "no-store response must not be considered cacheable");
        cacheResponse.headers = { { "Cache-Control", "max-age=10, max-age=20" } };
        RequireStatus(ParseHttpCacheControl(cacheResponse.headers),
            StatusCode::InvalidArgument,
            "conflicting cache freshness directives should be rejected");
    }

    void TestModernWebsiteSessionRoleBoundary() {
        using namespace LikesProgram::Http;

        HttpRequest browserRequest;
        browserRequest.method = "GET";
        browserRequest.target = "/index.html?lang=en";
        browserRequest.headers = {
            { "Host", "www.example.test" },
            { "Accept", "text/html,application/xhtml+xml" },
            { "Accept-Encoding", "gzip, br" },
            { "Cookie", "sid=opaque" },
            { "Range", "bytes=0-1023" },
            { "Connection", "keep-alive" }
        };

        const auto requestWire = BuildHttp1Request(browserRequest);
        Require(requestWire.IsOk(),
            "modern website request headers should remain valid HTTP/1.1 wire data");
        const auto parsedRequest = ParseHttp1Request(requestWire.Value());
        Require(parsedRequest.IsOk()
                && HttpHeaderValue(parsedRequest.Value().headers, "accept-encoding") == "gzip, br"
                && HttpHeaderValue(parsedRequest.Value().headers, "cookie") == "sid=opaque"
                && HttpHeaderValue(parsedRequest.Value().headers, "range") == "bytes=0-1023",
            "modern website request negotiation headers should round-trip");

        LoopbackTransport transport;
        HttpSession client(&transport);
        client.SetVersion(HttpVersion::Http1);
        Require(client.Send(browserRequest).IsOk()
                && transport.lastVersion == HttpVersion::Http1,
            "HTTPClientSession role should dispatch HTTP/1.1 through the injected transport");

        client.SetVersion(HttpVersion::Http3);
        Require(client.Send(browserRequest).IsOk()
                && transport.lastVersion == HttpVersion::Http3,
            "HTTPClientSession role should preserve HTTP/3 selection for the adapter");

        EchoHandler handler;
        HttpSession server(nullptr, &handler);
        const auto serverResponse = server.Handle(browserRequest);
        Require(serverResponse.IsOk()
                && serverResponse.Value().body.size() == browserRequest.target.size(),
            "HTTPServerSession role should dispatch the request to the handler");

        HttpResponse partialResponse;
        partialResponse.statusCode = 206;
        partialResponse.reason = "Partial Content";
        partialResponse.headers = {
            { "Content-Range", "bytes 0-3/8" },
            { "Content-Encoding", "br" },
            { "Set-Cookie", "sid=opaque; Secure; HttpOnly" }
        };
        partialResponse.body = { 'd', 'a', 't', 'a' };
        const auto responseWire = BuildHttp1Response(partialResponse);
        Require(responseWire.IsOk(),
            "modern website response metadata should remain valid HTTP/1.1 wire data");
        const auto parsedResponse = ParseHttp1Response(responseWire.Value());
        Require(parsedResponse.IsOk()
                && parsedResponse.Value().statusCode == 206
                && HttpHeaderValue(parsedResponse.Value().headers, "content-range")
                    == "bytes 0-3/8"
                && HttpHeaderValue(parsedResponse.Value().headers, "set-cookie")
                    == "sid=opaque; Secure; HttpOnly",
            "modern website response metadata should round-trip without policy claims");
    }

    void TestHttp3QuicAdapterContract() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actionRecorder;

        struct EventRecorder final : Http3QuicEventObserver {
            std::vector<Http3QuicEventKind> events;

            void Observe(const Http3QuicEvent& event) noexcept override {
                events.push_back(event.kind);
            }
        } eventRecorder;

        Http3QuicAdapter adapter;
        RequireStatus(adapter.SendStreamData(1, { 'x' }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should require handshake before actions");
        RequireStatus(adapter.AttachActionSink(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject a null action sink");
        Require(adapter.AttachActionSink(&actionRecorder).IsOk()
                && adapter.AttachActionSink(&actionRecorder).IsOk()
                && adapter.AttachEventObserver(&eventRecorder).IsOk()
                && adapter.AttachEventObserver(&eventRecorder).IsOk(),
            "HTTP/3 QUIC adapter should attach idempotent non-owning endpoints");
        Require(adapter.SetDeadline(LikesProgram::Time::Deadline::FromNow(
                    LikesProgram::Time::Duration::zero())).IsOk() == false
                && adapter.HasDeadline()
                && adapter.DeadlineExpired(),
            "HTTP/3 QUIC adapter should report an expired caller-owned deadline");
        RequireStatus(adapter.CheckDeadline(), LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP/3 QUIC adapter should expose deadline expiration without owning a timer");
        Require(adapter.LastErrorContext().deadlineExpired,
            "HTTP/3 QUIC adapter should expose deadline expiry in its error context");
        const auto deadlineContext = adapter.LastHttpErrorContext();
        Require(deadlineContext.valid
                && deadlineContext.version == HttpVersion::Http3
                && deadlineContext.scope == HttpErrorScope::Connection
                && deadlineContext.origin == HttpErrorOrigin::Lifecycle
                && deadlineContext.unitKind == HttpErrorUnitKind::None
                && deadlineContext.statusCode
                    == LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP/3 deadline failures should map into connection lifecycle context");
        Require(adapter.SetDeadline(LikesProgram::Time::Deadline::Infinite()).IsOk()
                && !adapter.LastErrorContext().deadlineExpired
                && !adapter.LastHttpErrorContext().valid,
            "HTTP/3 QUIC adapter should clear deadline context when the caller removes the deadline");

        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && adapter.State() == Http3QuicAdapterState::Ready,
            "HTTP/3 QUIC adapter should enter Ready after external handshake");
        RequireStatus(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject a duplicate handshake event");
        Require(adapter.State() == Http3QuicAdapterState::Failed
                && adapter.LastError().Code() == LikesProgram::StatusCode::FailedPrecondition
                && adapter.LastErrorContext().valid
                && adapter.LastErrorContext().eventKind
                    == Http3QuicEventKind::HandshakeComplete
                && adapter.LastErrorContext().streamId == 0
                && adapter.LastErrorContext().errorCode == 0,
            "HTTP/3 QUIC adapter should make peer ordering errors terminal");
        const auto genericH3Context = adapter.LastHttpErrorContext();
        Require(genericH3Context.valid
                && genericH3Context.version == HttpVersion::Http3
                && genericH3Context.scope == HttpErrorScope::Connection
                && genericH3Context.origin == HttpErrorOrigin::Protocol
                && genericH3Context.unitKind == HttpErrorUnitKind::Event
                && genericH3Context.unitType
                    == static_cast<std::uint64_t>(
                        Http3QuicEventKind::HandshakeComplete)
                && genericH3Context.statusCode
                    == LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 errors should map into the cross-version diagnostic context");

        adapter.Reset();
        actionRecorder.actions.clear();
        eventRecorder.events.clear();
        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC adapter should be reusable after a terminal peer error");
        Require(!adapter.LastErrorContext().valid,
            "HTTP/3 QUIC adapter reset should clear terminal error context");

        const std::vector<std::uint8_t> payload{ 'h', '3' };
        Require(adapter.SetDeadline(LikesProgram::Time::Deadline::FromNow(
                    LikesProgram::Time::Duration::zero())).IsOk() == false,
            "HTTP/3 QUIC adapter should accept an expired deadline for action checks");
        RequireStatus(adapter.SendStreamData(0, payload),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP/3 QUIC adapter should reject actions after deadline expiration");
        Require(adapter.Snapshot().actionCount == 0
                && adapter.SetDeadline(LikesProgram::Time::Deadline::Infinite()).IsOk(),
            "HTTP/3 QUIC adapter deadline recovery should not emit an action");
        Require(adapter.SendStreamData(0, payload).IsOk()
                && actionRecorder.actions.size() == 1
                && actionRecorder.actions.front().kind == Http3QuicActionKind::StreamData
                && actionRecorder.actions.front().streamId == 0
                && actionRecorder.actions.front().payload == payload,
            "HTTP/3 QUIC adapter should accept stream zero and hand data to the action sink");
        RequireStatus(adapter.SendStreamData(std::uint64_t{ 1 } << 62, payload),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject a local stream ID beyond 62 bits");
        Require(adapter.State() == Http3QuicAdapterState::Ready
                && adapter.Snapshot().actionCount == 1,
            "HTTP/3 QUIC adapter should keep local validation failures non-terminal");
        Require(adapter.SendStreamFin(0).IsOk()
                && actionRecorder.actions.back().kind == Http3QuicActionKind::StreamFin,
            "HTTP/3 QUIC adapter should hand FIN to the external action sink");
        RequireStatus(adapter.SendStreamData(0, payload),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject data after local FIN");

        Require(adapter.Feed({ Http3QuicEventKind::StreamData, 3, 0, payload }).IsOk()
                && adapter.Feed({ Http3QuicEventKind::StreamFin, 3, 0, {} }).IsOk()
                && eventRecorder.events.size() == 3,
            "HTTP/3 QUIC adapter should observe peer stream data and FIN");
        Require(adapter.Feed({ Http3QuicEventKind::StreamData, 7, 0, payload }).IsOk()
                && adapter.Feed({ Http3QuicEventKind::StreamFin, 7, 0, {} }).IsOk()
                && adapter.SendStreamData(7, payload).IsOk()
                && adapter.SendStreamFin(7).IsOk(),
            "HTTP/3 QUIC adapter should preserve local sending after peer FIN");
        Require(adapter.ResetStream(5, 0x10).IsOk()
                && adapter.StopSending(6, 0x11).IsOk(),
            "HTTP/3 QUIC adapter should expose reset and stop-sending actions");
        const Http3RequestStreamQuicActions actionPlan{ 0x10E, true, true };
        Require(adapter.ApplyRequestStreamError(8, actionPlan).IsOk()
                && actionRecorder.actions.size() == 8
                && actionRecorder.actions[actionRecorder.actions.size() - 2].kind
                    == Http3QuicActionKind::ResetStream
                && actionRecorder.actions.back().kind
                    == Http3QuicActionKind::StopSending,
            "HTTP/3 QUIC adapter should apply an H3 error plan with direction checks");
        RequireStatus(adapter.ApplyRequestStreamError(7, actionPlan),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject an H3 error plan after both directions close");
        Require(actionRecorder.actions.size() == 8,
            "HTTP/3 QUIC adapter should not emit a partial plan after direction rejection");
        RequireStatus(adapter.ApplyRequestStreamError(9,
                Http3RequestStreamQuicActions{ 0, true, true }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject an invalid H3 error action plan");
        const std::vector<std::uint8_t> closeReason{ 'c', 'l', 'o', 's', 'e' };
        Require(adapter.Close(0x100).IsOk()
                && adapter.State() == Http3QuicAdapterState::Closing
                && adapter.Feed({ Http3QuicEventKind::ConnectionClose,
                    0, 0x100, closeReason, 0, true }).IsOk()
                && adapter.State() == Http3QuicAdapterState::Closed,
            "HTTP/3 QUIC adapter should separate local close action from peer close event");
        Require(adapter.LastErrorContext().valid
                && adapter.LastErrorContext().applicationError
                && adapter.LastErrorContext().closeReasonSize == closeReason.size()
                && std::equal(closeReason.begin(), closeReason.end(),
                    adapter.LastErrorContext().closeReason.begin()),
            "HTTP/3 QUIC adapter should retain application close context");
        adapter.Reset();
        Require(adapter.LastErrorContext().closeReasonSize == 0,
            "HTTP/3 QUIC adapter reset should clear close reason context");
        RequireStatus(adapter.Feed({ Http3QuicEventKind::ConnectionClose, 0, 0x100, {} }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject a duplicate peer close event");

        adapter.Reset();
        Require(adapter.State() == Http3QuicAdapterState::AwaitingHandshake
                && adapter.Snapshot().actionCount == 0,
            "HTTP/3 QUIC adapter reset should retain endpoints and clear transcript state");
        Http3QuicAdapter moved(std::move(adapter));
        Require(adapter.State() == Http3QuicAdapterState::Failed
                && moved.State() == Http3QuicAdapterState::AwaitingHandshake,
            "HTTP/3 QUIC adapter move should leave a safe moved-from state");

        Http3QuicAdapter invalidPeerAdapter;
        Require(invalidPeerAdapter.AttachActionSink(&actionRecorder).IsOk()
                && invalidPeerAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC adapter should prepare the invalid peer ID regression");
        RequireStatus(invalidPeerAdapter.Feed({ Http3QuicEventKind::StreamData,
                std::uint64_t{ 1 } << 62, 0, payload }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject a peer stream ID beyond 62 bits");
        Require(invalidPeerAdapter.State() == Http3QuicAdapterState::Failed,
            "HTTP/3 QUIC adapter should make invalid peer stream IDs terminal");

        Http3QuicAdapter limitedAdapter(&actionRecorder);
        RequireStatus(limitedAdapter.SetLimits({ 0, 4, 4, 1 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject zero resource limits");
        Require(limitedAdapter.SetLimits({ 1, 4, 1, 1 }).IsOk()
                && limitedAdapter.Limits().maxActiveStreams == 1
                && limitedAdapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && limitedAdapter.Feed({ Http3QuicEventKind::StreamData,
                    0, 0, { 'x' } }).IsOk(),
            "HTTP/3 QUIC adapter should accept configured bounded resources");
        RequireStatus(limitedAdapter.Feed({ Http3QuicEventKind::StreamData,
                1, 0, { 'x' }}),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should reject a stream beyond the active limit");
        RequireStatus(limitedAdapter.SendStreamData(0, { 'x', 'y' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should reject an oversized action payload");
        Require(limitedAdapter.SendStreamData(0, { 'x' }).IsOk(),
            "HTTP/3 QUIC adapter should emit within the action budget");
        RequireStatus(limitedAdapter.SendStreamFin(0),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should reject an action beyond the action limit");

        ActionRecorder streamRollbackActions;
        Http3QuicAdapter streamRollbackAdapter;
        Require(streamRollbackAdapter.SetLimits({ 1, 4, 4, 16 }).IsOk()
                && streamRollbackAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC adapter should prepare active-stream rollback coverage");
        RequireStatus(streamRollbackAdapter.SendStreamData(60, { 'x' }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject a stream action without a sink");
        RequireStatus(streamRollbackAdapter.ApplyRequestStreamError(61,
                Http3RequestStreamQuicActions{ 0x10E, true, true }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject an error plan without a sink");
        Require(streamRollbackAdapter.Snapshot().activeStreams == 0
                && streamRollbackAdapter.AttachActionSink(
                    &streamRollbackActions).IsOk()
                && streamRollbackAdapter.SendStreamFin(62).IsOk()
                && streamRollbackAdapter.Snapshot().activeStreams == 1,
            "HTTP/3 QUIC local submission failures should release a new stream slot");

        ActionRecorder actionLimitRollbackActions;
        Http3QuicAdapter actionLimitRollbackAdapter(&actionLimitRollbackActions);
        Require(actionLimitRollbackAdapter.SetLimits({ 2, 4, 1, 16 }).IsOk()
                && actionLimitRollbackAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk()
                && actionLimitRollbackAdapter.SendStreamData(64, { 'a' }).IsOk(),
            "HTTP/3 QUIC adapter should consume its bounded action budget");
        RequireStatus(actionLimitRollbackAdapter.SendStreamFin(68),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC action-limit failure should reject a new stream locally");
        Require(actionLimitRollbackAdapter.Snapshot().activeStreams == 1
                && actionLimitRollbackActions.actions.size() == 1,
            "HTTP/3 QUIC action-limit failure should roll back the new stream slot");

        ActionRecorder feedbackSlotActions;
        HttpBodyBudget feedbackSlotBudget({ 8, 8 });
        Http3QuicAdapter feedbackSlotAdapter(&feedbackSlotActions);
        Require(feedbackSlotAdapter.SetLimits({ 1, 16, 16, 8 }).IsOk()
                && feedbackSlotAdapter.AttachBodyBudget(
                    &feedbackSlotBudget).IsOk()
                && feedbackSlotAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && feedbackSlotAdapter.Feed({
                    Http3QuicEventKind::ConnectionSendCredit,
                    0, 0, {}, 4 }).IsOk()
                && feedbackSlotAdapter.SendStreamData(
                    72, { 'a', 'b' }).IsOk(),
            "HTTP/3 QUIC first DATA should reserve one unified stream slot");
        const auto feedbackSlotActionId =
            feedbackSlotActions.actions.back().actionId;
        Require(feedbackSlotAdapter.Snapshot().activeStreams == 1
                && feedbackSlotAdapter.Snapshot().provisionalStreams == 1
                && feedbackSlotAdapter.Snapshot().pendingDataBytes == 2
                && feedbackSlotAdapter.Snapshot().pendingBodyBudgetBytes == 2
                && feedbackSlotAdapter.SendCredit(72).Value().connectionCredit == 2
                && feedbackSlotBudget.ReservedBytes(72) == 2,
            "HTTP/3 QUIC provisional snapshot should align slot, credit, and budget");
        RequireStatus(feedbackSlotAdapter.SendStreamData(76, { 'x' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC provisional slot should enforce the concurrent-stream limit");
        RequireStatus(feedbackSlotAdapter.FeedTransportFeedback({
                Http3QuicTransportFeedbackKind::ActionRejected,
                feedbackSlotActionId, 1, 0x71 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC partial rejection should preserve the unified reservation");
        Require(feedbackSlotAdapter.Snapshot().provisionalStreams == 1
                && feedbackSlotAdapter.SendCredit(72).Value().connectionCredit == 2
                && feedbackSlotBudget.ReservedBytes(72) == 2
                && feedbackSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    feedbackSlotActionId, 0, 0x71 }).IsOk()
                && feedbackSlotAdapter.Snapshot().activeStreams == 0
                && feedbackSlotAdapter.Snapshot().provisionalStreams == 0
                && feedbackSlotAdapter.Snapshot().pendingDataBytes == 0
                && feedbackSlotAdapter.SendCredit(72).Value().connectionCredit == 4
                && feedbackSlotBudget.ReservedBytes() == 0
                && feedbackSlotAdapter.SendStreamData(76, { 'x' }).IsOk(),
            "HTTP/3 QUIC rejected first DATA should restore every reservation dimension");
        Require(feedbackSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    feedbackSlotActions.actions.back().actionId, 1, 0 }).IsOk()
                && feedbackSlotAdapter.Snapshot().activeStreams == 1
                && feedbackSlotAdapter.Snapshot().provisionalStreams == 0,
            "HTTP/3 QUIC accepted replacement DATA should commit the stream slot");

        ActionRecorder transferSlotActions;
        Http3QuicAdapter transferSlotAdapter(&transferSlotActions);
        Require(transferSlotAdapter.SetLimits({ 1, 16, 16, 8 }).IsOk()
                && transferSlotAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && transferSlotAdapter.SendStreamData(80, { 'a' }).IsOk()
                && transferSlotAdapter.SendStreamData(80, { 'b' }).IsOk(),
            "HTTP/3 QUIC provisional ownership transfer fixture should submit two DATA actions");
        const auto transferFirstId = transferSlotActions.actions[
            transferSlotActions.actions.size() - 2].actionId;
        const auto transferSecondId = transferSlotActions.actions.back().actionId;
        Require(transferSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    transferFirstId, 0, 0x72 }).IsOk()
                && transferSlotAdapter.Snapshot().activeStreams == 1
                && transferSlotAdapter.Snapshot().provisionalStreams == 1
                && transferSlotAdapter.Snapshot().pendingDataActions == 1
                && transferSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    transferSecondId, 0, 0x73 }).IsOk()
                && transferSlotAdapter.Snapshot().activeStreams == 0
                && transferSlotAdapter.Snapshot().provisionalStreams == 0,
            "HTTP/3 QUIC rejection should transfer then release provisional ownership");

        ActionRecorder acceptedOutOfOrderActions;
        Http3QuicAdapter acceptedOutOfOrderAdapter(&acceptedOutOfOrderActions);
        Require(acceptedOutOfOrderAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && acceptedOutOfOrderAdapter.SendStreamData(84, { 'a' }).IsOk()
                && acceptedOutOfOrderAdapter.SendStreamData(84, { 'b' }).IsOk(),
            "HTTP/3 QUIC out-of-order acceptance fixture should submit two DATA actions");
        const auto acceptedEarlierId = acceptedOutOfOrderActions.actions[
            acceptedOutOfOrderActions.actions.size() - 2].actionId;
        const auto acceptedLaterId = acceptedOutOfOrderActions.actions.back().actionId;
        Require(acceptedOutOfOrderAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    acceptedLaterId, 1, 0 }).IsOk()
                && acceptedOutOfOrderAdapter.Snapshot().provisionalStreams == 0
                && acceptedOutOfOrderAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    acceptedEarlierId, 0, 0x74 }).IsOk()
                && acceptedOutOfOrderAdapter.Snapshot().activeStreams == 1
                && acceptedOutOfOrderAdapter.Snapshot().provisionalStreams == 0,
            "HTTP/3 QUIC later acceptance should commit the slot before earlier rejection");

        ActionRecorder peerObservedSlotActions;
        Http3QuicAdapter peerObservedSlotAdapter(&peerObservedSlotActions);
        Require(peerObservedSlotAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && peerObservedSlotAdapter.SendStreamData(88, { 'a' }).IsOk(),
            "HTTP/3 QUIC peer-observed slot fixture should submit first DATA");
        const auto peerObservedSlotId = peerObservedSlotActions.actions.back().actionId;
        Require(peerObservedSlotAdapter.Feed({ Http3QuicEventKind::StreamData,
                    88, 0, { 'r' } }).IsOk()
                && peerObservedSlotAdapter.Snapshot().provisionalStreams == 0
                && peerObservedSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    peerObservedSlotId, 0, 0x75 }).IsOk()
                && peerObservedSlotAdapter.Snapshot().activeStreams == 1,
            "HTTP/3 QUIC peer observation should commit a rejected outbound slot");

        ActionRecorder terminalSlotActions;
        Http3QuicAdapter terminalSlotAdapter(&terminalSlotActions);
        Require(terminalSlotAdapter.SetLimits({ 1, 16, 16, 8 }).IsOk()
                && terminalSlotAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && terminalSlotAdapter.SendStreamFin(92).IsOk()
                && terminalSlotAdapter.Snapshot().provisionalStreams == 1,
            "HTTP/3 QUIC first FIN should own a provisional stream slot");
        Require(terminalSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    terminalSlotActions.actions.back().actionId, 0, 0x76 }).IsOk()
                && terminalSlotAdapter.Snapshot().activeStreams == 0
                && terminalSlotAdapter.Snapshot().provisionalStreams == 0
                && terminalSlotAdapter.StopSending(96, 0x10E).IsOk(),
            "HTTP/3 QUIC rejected first FIN should release its stream slot");
        Require(terminalSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    terminalSlotActions.actions.back().actionId, 0, 0x77 }).IsOk()
                && terminalSlotAdapter.Snapshot().activeStreams == 0
                && terminalSlotAdapter.Snapshot().provisionalStreams == 0
                && terminalSlotAdapter.ResetStream(100, 0x10E).IsOk(),
            "HTTP/3 QUIC rejected first STOP_SENDING should release its stream slot");
        Require(terminalSlotAdapter.Snapshot().provisionalStreams == 1
                && terminalSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    terminalSlotActions.actions.back().actionId, 0, 0x78 }).IsOk()
                && terminalSlotAdapter.Snapshot().activeStreams == 0
                && terminalSlotAdapter.Snapshot().provisionalStreams == 0,
            "HTTP/3 QUIC rejected first RESET_STREAM should release its ledger entry");

        ActionRecorder movedSlotActions;
        Http3QuicAdapter movedSlotSource(&movedSlotActions);
        Require(movedSlotSource.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && movedSlotSource.SendStreamData(104, { 'm' }).IsOk(),
            "HTTP/3 QUIC move fixture should own one provisional stream slot");
        const auto movedSlotActionId = movedSlotActions.actions.back().actionId;
        Http3QuicAdapter movedSlotAdapter(std::move(movedSlotSource));
        Require(movedSlotSource.Snapshot().provisionalStreams == 0
                && movedSlotAdapter.Snapshot().provisionalStreams == 1
                && movedSlotAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    movedSlotActionId, 0, 0x79 }).IsOk()
                && movedSlotAdapter.Snapshot().activeStreams == 0
                && movedSlotAdapter.Snapshot().provisionalStreams == 0,
            "HTTP/3 QUIC move should transfer provisional ownership and rejection rollback");

        Http3QuicAdapter creditAdapter(&actionRecorder);
        const auto unboundedCredit = creditAdapter.SendCredit(12);
        Require(unboundedCredit.IsOk()
                && !unboundedCredit.Value().bounded
                && !unboundedCredit.Value().blocked
                && unboundedCredit.Value().availableCredit == 0,
            "HTTP/3 QUIC adapter should distinguish unbounded transport credit");
        RequireStatus(creditAdapter.SendCredit(std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject a credit query beyond 62 bits");
        Require(creditAdapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC adapter should accept an external flow-credit handshake");
        Require(creditAdapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                    12, 0, {}, 3 }).IsOk(),
            "HTTP/3 QUIC adapter should accept caller-owned stream send credit");
        RequireStatus(creditAdapter.SendStreamData(12, { 'a', 'b', 'c', 'd' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should pause data above external stream credit");
        Require(creditAdapter.SendStreamData(12, { 'a', 'b', 'c' }).IsOk(),
            "HTTP/3 QUIC adapter should consume available stream credit");
        Require(creditAdapter.Snapshot().trackedStreamCredits == 1
                && creditAdapter.Snapshot().zeroStreamCredits == 1,
            "HTTP/3 QUIC adapter snapshot should expose exhausted stream credit");
        const auto blockedCredit = creditAdapter.SendCredit(12);
        Require(blockedCredit.IsOk()
                && blockedCredit.Value().streamCreditActive
                && blockedCredit.Value().bounded
                && blockedCredit.Value().blocked
                && blockedCredit.Value().availableCredit == 0,
            "HTTP/3 QUIC adapter should expose an exhausted effective stream credit");
        RequireStatus(creditAdapter.SendStreamData(12, { 'd' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should retain zero stream credit after send");
        creditAdapter.Reset();
        Require(creditAdapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && creditAdapter.Feed({ Http3QuicEventKind::ConnectionSendCredit,
                    0, 0, {}, 4 }).IsOk()
                && creditAdapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                    12, 0, {}, 8 }).IsOk()
                && creditAdapter.SendStreamData(12, { 'x', 'y', 'z' }).IsOk(),
            "HTTP/3 QUIC adapter reset should clear external stream credit");
        Require(creditAdapter.Snapshot().connectionSendCreditActive
                && creditAdapter.Snapshot().connectionSendCredit == 1
                && creditAdapter.Snapshot().trackedStreamCredits == 1
                && creditAdapter.Snapshot().zeroStreamCredits == 0,
            "HTTP/3 QUIC adapter snapshot should expose connection and stream credit");
        const auto effectiveCredit = creditAdapter.SendCredit(12);
        Require(effectiveCredit.IsOk()
                && effectiveCredit.Value().connectionCreditActive
                && effectiveCredit.Value().connectionCredit == 1
                && effectiveCredit.Value().streamCreditActive
                && effectiveCredit.Value().streamCredit == 5
                && effectiveCredit.Value().bounded
                && !effectiveCredit.Value().blocked
                && effectiveCredit.Value().availableCredit == 1,
            "HTTP/3 QUIC adapter should expose the lower effective transport credit");
        RequireStatus(creditAdapter.SendStreamData(12, { 'a', 'b' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should enforce connection credit across streams");
        Require(creditAdapter.Feed({ Http3QuicEventKind::ConnectionSendCredit,
                    0, 0, {}, 2 }).IsOk()
                && creditAdapter.SendStreamData(12, { 'a', 'b' }).IsOk(),
            "HTTP/3 QUIC adapter should accept refreshed connection credit");
        const auto consumedCredit = creditAdapter.SendCredit(12);
        Require(consumedCredit.IsOk()
                && consumedCredit.Value().connectionCredit == 0
                && consumedCredit.Value().streamCredit == 3
                && consumedCredit.Value().blocked,
            "HTTP/3 QUIC adapter credit query should follow successful send consumption");
        creditAdapter.Reset();
        Require(creditAdapter.SendCredit(12).IsOk()
                && !creditAdapter.SendCredit(12).Value().bounded,
            "HTTP/3 QUIC adapter reset should clear effective send credit");
        RequireStatus(adapter.SendCredit(0), LikesProgram::StatusCode::Internal,
            "HTTP/3 QUIC adapter should reject moved-from send-credit queries");

        ActionRecorder feedbackActions;
        Http3QuicAdapter feedbackAdapter(&feedbackActions);
        Require(feedbackAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk()
                && feedbackAdapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                    44, 0, {}, 8 }).IsOk()
                && feedbackAdapter.Feed({ Http3QuicEventKind::ConnectionSendCredit,
                    0, 0, {}, 8 }).IsOk(),
            "HTTP/3 QUIC adapter should prepare transport feedback credit");
        Require(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Blocked }).IsOk()
                && feedbackAdapter.Snapshot().transportBlocked,
            "HTTP/3 QUIC adapter should expose caller-owned transport blocking");
        RequireStatus(feedbackAdapter.SendStreamData(44, { 'b' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should reject DATA while transport is blocked");
        Require(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable }).IsOk()
                && !feedbackAdapter.Snapshot().transportBlocked,
            "HTTP/3 QUIC adapter should resume DATA after writable feedback");
        Require(feedbackAdapter.SendStreamData(44, { 'a', 'b', 'c' }).IsOk()
                && feedbackActions.actions.size() == 1
                && feedbackActions.actions.back().actionId != 0
                && feedbackAdapter.Snapshot().pendingDataActions == 1,
            "HTTP/3 QUIC adapter should expose a pending DATA action id");
        const auto acceptedId = feedbackActions.actions.back().actionId;
        Require(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    acceptedId, 3, 0 }).IsOk()
                && feedbackAdapter.Snapshot().pendingDataActions == 0
                && feedbackAdapter.SendCredit(44).Value().availableCredit == 5,
            "HTTP/3 QUIC adapter should retire an accepted DATA action");

        Require(feedbackAdapter.SendStreamData(44, { 'd', 'e' }).IsOk()
                && feedbackActions.actions.back().actionId != acceptedId,
            "HTTP/3 QUIC adapter should allocate a distinct id per DATA action");
        const auto rejectedId = feedbackActions.actions.back().actionId;
        RequireStatus(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    rejectedId, 1, 0 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject partial DATA acceptance");
        Require(feedbackAdapter.Snapshot().pendingDataActions == 1
                && feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    rejectedId, 0, 0 }).IsOk()
                && feedbackAdapter.Snapshot().pendingDataActions == 0
                && feedbackAdapter.SendCredit(44).Value().availableCredit == 5,
            "HTTP/3 QUIC adapter should restore reserved credit after rejection");
        Require(feedbackAdapter.LastErrorContext().transportFeedbackValid
                && feedbackAdapter.LastErrorContext().transportFeedbackKind
                    == Http3QuicTransportFeedbackKind::ActionRejected
                && feedbackAdapter.LastErrorContext().transportActionId == rejectedId,
            "HTTP/3 QUIC adapter should retain rejected transport feedback context");
        RequireStatus(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    rejectedId, 0, 0 }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject duplicate transport feedback");
        Require(feedbackAdapter.SendStreamData(44, { 'f' }).IsOk()
                && feedbackAdapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                    44, 0, {}, 5 }).IsOk(),
            "HTTP/3 QUIC adapter should retain a new peer credit generation");
        const auto generationChangedId = feedbackActions.actions.back().actionId;
        Require(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    generationChangedId, 0, 0 }).IsOk()
                && feedbackAdapter.SendCredit(44).Value().streamCredit == 5
                && feedbackAdapter.SendCredit(44).Value().connectionCredit == 5,
            "HTTP/3 QUIC adapter should not restore credit across a peer update");
        RequireStatus(feedbackAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable,
                    1, 0, 0 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should validate transport state feedback coordinates");
        feedbackAdapter.Reset();
        Require(!feedbackAdapter.Snapshot().transportBlocked
                && feedbackAdapter.Snapshot().pendingDataActions == 0
                && !feedbackAdapter.LastErrorContext().transportFeedbackValid,
            "HTTP/3 QUIC adapter reset should clear transport feedback state");

        ActionRecorder cancellationActions;
        Http3QuicAdapter cancellationAdapter(&cancellationActions);
        Require(cancellationAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk()
                && cancellationAdapter.ApplyBodyCancellation(
                    20, HttpBodyCancelReason::Application).IsOk()
                && cancellationActions.actions.size() == 2
                && cancellationActions.actions[0].kind
                    == Http3QuicActionKind::ResetStream
                && cancellationActions.actions[1].kind
                    == Http3QuicActionKind::StopSending
                && cancellationActions.actions[0].errorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled)
                && cancellationActions.actions[1].errorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled),
            "HTTP/3 QUIC adapter should map application cancellation to both open directions");
        const auto cancellationResetId = cancellationActions.actions[0].actionId;
        const auto cancellationStopId = cancellationActions.actions[1].actionId;
        Require(cancellationResetId != 0
                && cancellationStopId != 0
                && cancellationResetId != cancellationStopId
                && cancellationAdapter.Snapshot().pendingControlActions == 2,
            "HTTP/3 QUIC cancellation should expose distinct pending control actions");
        RequireStatus(cancellationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    cancellationResetId, 1, 0 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC control feedback should reject accepted byte counts");
        Require(cancellationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    cancellationResetId, 0, 0 }).IsOk()
                && cancellationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    cancellationStopId, 0, 0x77 }).IsOk()
                && cancellationAdapter.Snapshot().pendingControlActions == 0,
            "HTTP/3 QUIC cancellation feedback should retire accepted and rejected actions");
        Require(cancellationAdapter.StopSending(
                    20, static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled)).IsOk()
                && cancellationActions.actions.back().actionId != cancellationStopId,
            "HTTP/3 QUIC rejected STOP_SENDING should reopen its local direction for retry");
        const auto retryStopId = cancellationActions.actions.back().actionId;
        RequireStatus(cancellationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    retryStopId, 0, 1 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC accepted control feedback should reject transport error codes");
        Require(cancellationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    retryStopId, 0, 0 }).IsOk(),
            "HTTP/3 QUIC retry STOP_SENDING should be acknowledgeable");
        ActionRecorder resetControlActions;
        Http3QuicAdapter resetControlAdapter(&resetControlActions);
        Require(resetControlAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk()
                && resetControlAdapter.ApplyBodyCancellation(
                    21, HttpBodyCancelReason::Application).IsOk()
                && resetControlAdapter.Snapshot().pendingControlActions == 2,
            "HTTP/3 QUIC reset regression should create pending cancellation actions");
        resetControlAdapter.Reset();
        Require(resetControlAdapter.Snapshot().pendingControlActions == 0
                && resetControlActions.actions.size() == 2,
            "HTTP/3 QUIC reset should clear pending control feedback state");
        ActionRecorder terminalActions;
        Http3QuicAdapter terminalAdapter(&terminalActions);
        Require(terminalAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk()
                && terminalAdapter.SendStreamFin(50).IsOk()
                && terminalAdapter.Snapshot().pendingTerminalActions == 1,
            "HTTP/3 QUIC terminal feedback should track a pending FIN");
        const auto finActionId = terminalActions.actions.back().actionId;
        RequireStatus(terminalAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    finActionId, 1, 0 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC FIN feedback should reject accepted byte counts");
        Require(terminalAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    finActionId, 0, 0x77 }).IsOk()
                && terminalAdapter.Snapshot().pendingTerminalActions == 0
                && terminalAdapter.SendStreamData(50, { 'r' }).IsOk(),
            "HTTP/3 QUIC rejected FIN should reopen its local direction");
        Require(terminalAdapter.Close(0x99).IsOk()
                && terminalAdapter.State() == Http3QuicAdapterState::Closing
                && terminalAdapter.Snapshot().pendingTerminalActions == 1,
            "HTTP/3 QUIC terminal feedback should track a pending close");
        const auto closeActionId = terminalActions.actions.back().actionId;
        Require(terminalAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    closeActionId, 0, 0x88 }).IsOk()
                && terminalAdapter.State() == Http3QuicAdapterState::Ready
                && terminalAdapter.Snapshot().pendingTerminalActions == 0
                && terminalAdapter.Close(0x99).IsOk(),
            "HTTP/3 QUIC rejected close should reopen the ready state");
        const auto acceptedCloseId = terminalActions.actions.back().actionId;
        Require(terminalAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    acceptedCloseId, 0, 0 }).IsOk()
                && terminalAdapter.State() == Http3QuicAdapterState::Closing,
            "HTTP/3 QUIC accepted close should retain the closing state");

        ActionRecorder deadlineCloseActions;
        Http3QuicAdapter deadlineCloseAdapter(&deadlineCloseActions);
        Require(deadlineCloseAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC deadline-close adapter should become ready");
        RequireStatus(deadlineCloseAdapter.CloseExpiredDeadline(
                static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled)),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC live deadline must not emit a connection close");
        Require(deadlineCloseActions.actions.empty()
                && deadlineCloseAdapter.State() == Http3QuicAdapterState::Ready
                && deadlineCloseAdapter.Snapshot().pendingTerminalActions == 0,
            "HTTP/3 QUIC live deadline rejection should not mutate close state");

        RequireStatus(deadlineCloseAdapter.SetDeadline(
                LikesProgram::Time::Deadline::FromNow(
                    LikesProgram::Time::Duration::zero())),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP/3 QUIC deadline-close test should observe caller expiry");
        const auto deadlineError = static_cast<std::uint64_t>(
            Http3ErrorCode::RequestCancelled);
        Require(deadlineCloseAdapter.CloseExpiredDeadline(deadlineError).IsOk()
                && deadlineCloseActions.actions.size() == 1
                && deadlineCloseActions.actions.back().kind
                    == Http3QuicActionKind::CloseConnection
                && deadlineCloseActions.actions.back().errorCode == deadlineError
                && deadlineCloseAdapter.State() == Http3QuicAdapterState::Closing
                && deadlineCloseAdapter.Snapshot().pendingTerminalActions == 1
                && deadlineCloseAdapter.LastErrorContext().deadlineExpired,
            "HTTP/3 QUIC expired deadline should hand off the caller-selected close");
        const auto rejectedDeadlineCloseId =
            deadlineCloseActions.actions.back().actionId;
        Require(deadlineCloseAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    rejectedDeadlineCloseId, 0, 0x90 }).IsOk()
                && deadlineCloseAdapter.State() == Http3QuicAdapterState::Ready
                && deadlineCloseAdapter.Snapshot().pendingTerminalActions == 0
                && deadlineCloseAdapter.DeadlineExpired()
                && deadlineCloseAdapter.CloseExpiredDeadline(deadlineError).IsOk()
                && deadlineCloseActions.actions.back().actionId
                    != rejectedDeadlineCloseId,
            "HTTP/3 QUIC rejected deadline close should remain expired and retryable");
        const auto acceptedDeadlineCloseId =
            deadlineCloseActions.actions.back().actionId;
        Require(deadlineCloseAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    acceptedDeadlineCloseId, 0, 0 }).IsOk()
                && deadlineCloseAdapter.State() == Http3QuicAdapterState::Closing,
            "HTTP/3 QUIC accepted deadline close should retain closing state");
        deadlineCloseAdapter.Reset();
        Require(deadlineCloseAdapter.State()
                    == Http3QuicAdapterState::AwaitingHandshake
                && !deadlineCloseAdapter.HasDeadline()
                && !deadlineCloseAdapter.DeadlineExpired()
                && !deadlineCloseAdapter.LastErrorContext().deadlineExpired
                && deadlineCloseAdapter.Snapshot().pendingTerminalActions == 0,
            "HTTP/3 QUIC reset should clear deadline-close transaction state");
        Require(cancellationAdapter.SendStreamFin(24).IsOk()
                && cancellationAdapter.ApplyBodyCancellation(
                    24, HttpBodyCancelReason::DeadlineExceeded).IsOk()
                && cancellationActions.actions.back().kind
                    == Http3QuicActionKind::StopSending
                && cancellationActions.actions.back().errorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::RequestCancelled),
            "HTTP/3 QUIC adapter should cancel only the open direction after local FIN");
        Require(cancellationAdapter.Feed(
                    { Http3QuicEventKind::StreamFin, 28 }).IsOk()
                && cancellationAdapter.ApplyBodyCancellation(
                    28, HttpBodyCancelReason::ProtocolError).IsOk()
                && cancellationActions.actions.back().kind
                    == Http3QuicActionKind::ResetStream
                && cancellationActions.actions.back().errorCode
                    == static_cast<std::uint64_t>(Http3ErrorCode::MessageError),
            "HTTP/3 QUIC adapter should map protocol cancellation on the open send direction");
        const auto actionsBeforePeerReset = cancellationActions.actions.size();
        Require(cancellationAdapter.Feed(
                    { Http3QuicEventKind::StreamReset, 32, 0x10C }).IsOk()
                && cancellationAdapter.ApplyBodyCancellation(
                    32, HttpBodyCancelReason::PeerReset).IsOk()
                && cancellationActions.actions.size() == actionsBeforePeerReset,
            "HTTP/3 QUIC adapter should not echo a peer reset as a local action");
        RequireStatus(cancellationAdapter.ApplyBodyCancellation(
                36, HttpBodyCancelReason::PeerReset),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should require evidence for peer-reset cancellation");
        RequireStatus(cancellationAdapter.ApplyBodyCancellation(36,
                static_cast<HttpBodyCancelReason>(0xFF)),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject an unknown body cancellation reason");

        ActionRecorder atomicActions;
        Http3QuicAdapter atomicCancellationAdapter(&atomicActions);
        Require(atomicCancellationAdapter.SetLimits({ 4, 4, 1, 16 }).IsOk()
                && atomicCancellationAdapter.Feed(
                    { Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC adapter should prepare bounded cancellation actions");
        RequireStatus(atomicCancellationAdapter.ApplyBodyCancellation(
                40, HttpBodyCancelReason::Application),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should reject a two-action cancellation atomically");
        Require(atomicActions.actions.empty()
                && atomicCancellationAdapter.Snapshot().actionCount == 0,
            "HTTP/3 QUIC adapter should not submit a partial cancellation plan");
    }

    void TestHttp3QuicReceiveCreditTransactions() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;

        Http3QuicAdapter adapter(&actions);
        RequireStatus(adapter.UpdateConnectionReceiveCredit(100),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 receive credit should require a completed handshake");
        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && adapter.UpdateConnectionReceiveCredit(100).IsOk(),
            "HTTP/3 connection receive-credit fixture should submit an update");
        const auto firstConnectionId = actions.actions.back().actionId;
        const auto pendingConnection = adapter.ReceiveCredit(8);
        Require(actions.actions.back().kind
                    == Http3QuicActionKind::ConnectionReceiveCredit
                && actions.actions.back().streamId == 0
                && actions.actions.back().flowCredit == 100
                && actions.actions.back().payload.empty()
                && pendingConnection.IsOk()
                && !pendingConnection.Value().connectionCreditSet
                && pendingConnection.Value().connectionUpdatePending
                && pendingConnection.Value().pendingConnectionCredit == 100
                && adapter.Snapshot().pendingReceiveCreditActions == 1,
            "HTTP/3 connection receive credit should remain pending before feedback");
        RequireStatus(adapter.UpdateConnectionReceiveCredit(120),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive credit should allow one pending update");
        RequireStatus(adapter.FeedTransportFeedback({
                Http3QuicTransportFeedbackKind::ActionRejected,
                firstConnectionId, 1, 7 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 receive-credit rejection should reject byte coordinates");
        Require(adapter.ReceiveCredit(8).Value().connectionUpdatePending
                && !adapter.LastErrorContext().transportFeedbackValid,
            "HTTP/3 invalid receive-credit feedback should preserve all state");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    firstConnectionId, 0, 0 }).IsOk(),
            "HTTP/3 connection receive credit should accept valid feedback");
        const auto committedConnection = adapter.ReceiveCredit(8);
        Require(committedConnection.IsOk()
                && committedConnection.Value().connectionCreditSet
                && committedConnection.Value().connectionCredit == 100
                && !committedConnection.Value().connectionUpdatePending
                && adapter.Snapshot().pendingReceiveCreditActions == 0,
            "HTTP/3 accepted connection receive credit should commit exactly once");
        RequireStatus(adapter.FeedTransportFeedback({
                Http3QuicTransportFeedbackKind::ActionAccepted,
                firstConnectionId, 0, 0 }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 receive credit should reject duplicate feedback");
        RequireStatus(adapter.UpdateConnectionReceiveCredit(100),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive credit should reject duplicates");
        RequireStatus(adapter.UpdateConnectionReceiveCredit(99),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive credit should reject regressions");

        Require(adapter.UpdateConnectionReceiveCredit(150).IsOk(),
            "HTTP/3 connection receive credit should submit an increase");
        const auto rejectedConnectionId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    rejectedConnectionId, 0, 0x71 }).IsOk(),
            "HTTP/3 connection receive credit should accept valid rejection feedback");
        const auto rejectedConnection = adapter.ReceiveCredit(8);
        Require(rejectedConnection.Value().connectionCredit == 100
                && !rejectedConnection.Value().connectionUpdatePending
                && adapter.LastErrorContext().transportFeedbackValid
                && adapter.LastErrorContext().transportActionId
                    == rejectedConnectionId
                && adapter.UpdateConnectionReceiveCredit(150).IsOk(),
            "HTTP/3 rejected connection credit should preserve the prior limit for retry");
        const auto retriedConnectionId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    retriedConnectionId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(8).Value().connectionCredit == 150,
            "HTTP/3 retried connection receive credit should commit after acceptance");

        Require(adapter.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, { 'x' } }).IsOk()
                && adapter.UpdateStreamReceiveCredit(8, 32).IsOk(),
            "HTTP/3 stream receive-credit fixture should create a receiving stream");
        const auto firstStreamId = actions.actions.back().actionId;
        const auto pendingStream = adapter.ReceiveCredit(8);
        Require(actions.actions.back().kind
                    == Http3QuicActionKind::StreamReceiveCredit
                && actions.actions.back().streamId == 8
                && actions.actions.back().flowCredit == 32
                && pendingStream.Value().streamUpdatePending
                && pendingStream.Value().pendingStreamCredit == 32
                && adapter.Snapshot().pendingReceiveCreditActions == 1,
            "HTTP/3 stream receive credit should expose its pending absolute limit");
        RequireStatus(adapter.UpdateStreamReceiveCredit(8, 64),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 stream receive credit should allow one pending update per stream");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    firstStreamId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(8).Value().streamCreditSet
                && adapter.ReceiveCredit(8).Value().streamCredit == 32,
            "HTTP/3 accepted stream receive credit should commit");
        RequireStatus(adapter.UpdateStreamReceiveCredit(8, 32),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive credit should reject duplicates");
        Require(adapter.UpdateStreamReceiveCredit(8, 64).IsOk(),
            "HTTP/3 stream receive credit should submit an increase");
        const auto rejectedStreamId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    rejectedStreamId, 0, 0x72 }).IsOk()
                && adapter.ReceiveCredit(8).Value().streamCredit == 32
                && !adapter.ReceiveCredit(8).Value().streamUpdatePending
                && adapter.UpdateStreamReceiveCredit(8, 64).IsOk(),
            "HTTP/3 rejected stream credit should preserve the prior limit for retry");
        const auto retriedStreamId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    retriedStreamId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(8).Value().streamCredit == 64,
            "HTTP/3 retried stream receive credit should commit after acceptance");

        Require(adapter.UpdateConnectionReceiveCredit(200).IsOk()
                && adapter.UpdateStreamReceiveCredit(8, 96).IsOk()
                && adapter.Snapshot().pendingReceiveCreditActions == 2,
            "HTTP/3 connection and stream receive credits should pend independently");
        const auto crossStreamId = actions.actions.back().actionId;
        const auto crossConnectionId = actions.actions[actions.actions.size() - 2].actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    crossStreamId, 0, 0 }).IsOk()
                && adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    crossConnectionId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(8).Value().connectionCredit == 200
                && adapter.ReceiveCredit(8).Value().streamCredit == 96,
            "HTTP/3 receive-credit scopes should accept feedback independently");
        RequireStatus(adapter.UpdateConnectionReceiveCredit(0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive credit should reject zero");
        RequireStatus(adapter.UpdateConnectionReceiveCredit(
                std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive credit should enforce the 62-bit limit");
        RequireStatus(adapter.UpdateStreamReceiveCredit(
                8, std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive credit should enforce the 62-bit limit");
        RequireStatus(adapter.UpdateStreamReceiveCredit(
                std::uint64_t{ 1 } << 62, 128),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive credit should reject an invalid stream id");
        RequireStatus(adapter.ReceiveCredit(std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 receive-credit query should reject an invalid stream id");
        Require(adapter.Feed({ Http3QuicEventKind::StreamFin, 8 }).IsOk(),
            "HTTP/3 receive-credit fixture should finish its peer stream");
        RequireStatus(adapter.UpdateStreamReceiveCredit(8, 128),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 stream receive credit should reject a peer-terminal stream");

        ActionRecorder missingSinkActions;
        Http3QuicAdapter missingSink;
        Require(missingSink.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 missing-sink receive-credit fixture should complete handshake");
        RequireStatus(missingSink.UpdateConnectionReceiveCredit(10),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 receive credit should require an action sink");
        Require(missingSink.Snapshot().actionCount == 0
                && missingSink.Snapshot().pendingReceiveCreditActions == 0
                && missingSink.AttachActionSink(&missingSinkActions).IsOk()
                && missingSink.UpdateConnectionReceiveCredit(10).IsOk()
                && missingSinkActions.actions.back().actionId == 1,
            "HTTP/3 local receive-credit failure should not consume an action id");

        ActionRecorder limitedActions;
        Http3QuicAdapter limited(&limitedActions);
        Require(limited.SetLimits({ 2, 8, 1, 16 }).IsOk()
                && limited.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && limited.UpdateConnectionReceiveCredit(10).IsOk(),
            "HTTP/3 limited receive-credit fixture should consume one action");
        const auto limitedConnectionId = limitedActions.actions.back().actionId;
        Require(limited.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    limitedConnectionId, 0, 0 }).IsOk()
                && limited.Feed({ Http3QuicEventKind::StreamData,
                    4, 0, { 'x' } }).IsOk(),
            "HTTP/3 limited receive-credit fixture should create a stream");
        RequireStatus(limited.UpdateStreamReceiveCredit(4, 10),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 receive credit should enforce the action limit");
        Require(!limited.ReceiveCredit(4).Value().streamCreditSet
                && !limited.ReceiveCredit(4).Value().streamUpdatePending
                && limited.Snapshot().pendingReceiveCreditActions == 0,
            "HTTP/3 action-limit failure should roll back stream credit state");

        ActionRecorder blockedActions;
        Http3QuicAdapter blocked(&blockedActions);
        Require(blocked.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && blocked.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Blocked }).IsOk(),
            "HTTP/3 blocked receive-credit fixture should observe transport state");
        RequireStatus(blocked.UpdateConnectionReceiveCredit(10),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 receive credit should not submit while transport-blocked");
        Require(blocked.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable }).IsOk()
                && blocked.UpdateConnectionReceiveCredit(10).IsOk(),
            "HTTP/3 receive credit should submit after transport resumes");

        ActionRecorder moveActions;
        Http3QuicAdapter moving(&moveActions);
        Require(moving.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && moving.UpdateConnectionReceiveCredit(77).IsOk(),
            "HTTP/3 moving receive-credit fixture should retain a pending update");
        const auto movedActionId = moveActions.actions.back().actionId;
        Http3QuicAdapter live(std::move(moving));
        RequireStatus(moving.UpdateConnectionReceiveCredit(88),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from adapter should reject receive-credit updates");
        RequireStatus(moving.ReceiveCredit(0),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from adapter should reject receive-credit queries");
        Require(live.ReceiveCredit(0).Value().connectionUpdatePending
                && live.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    movedActionId, 0, 0 }).IsOk()
                && live.ReceiveCredit(0).Value().connectionCredit == 77,
            "HTTP/3 adapter move should transfer pending receive-credit state");
        live.Reset();
        Require(!live.ReceiveCredit(0).Value().connectionCreditSet
                && !live.ReceiveCredit(0).Value().connectionUpdatePending
                && live.Snapshot().pendingReceiveCreditActions == 0,
            "HTTP/3 adapter Reset should clear receive-credit state");
    }

    void TestHttp3RequestSinkStreamReceiveCreditRefresh() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;

        const auto headers = BuildHttp3Frame({
            static_cast<std::uint64_t>(Http3FrameType::Headers), { 0x01 } });
        const auto data = BuildHttp3Frame({
            static_cast<std::uint64_t>(Http3FrameType::Data),
            { 'a', 'b', 'c', 'd' } });
        const auto laterData = BuildHttp3Frame({
            static_cast<std::uint64_t>(Http3FrameType::Data), { 'e', 'f' } });
        Require(headers.IsOk() && data.IsOk() && laterData.IsOk(),
            "HTTP/3 stream receive-credit bridge fixtures should build");

        Http3QuicRequestStreamBridge missingSink(8);
        RequireStatus(missingSink.ConfigureStreamReceiveCredit(100),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 stream receive-credit configuration should require a sink");

        HttpBodySink sink({ 8, 2, 8 });
        Http3QuicRequestStreamBridge bridge(8);
        Http3QuicAdapter adapter(&actions);
        Require(bridge.AttachBodySink(&sink).IsOk(),
            "HTTP/3 stream receive-credit bridge should attach a body sink");
        RequireStatus(bridge.ConfigureStreamReceiveCredit(0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive-credit configuration should reject zero");
        RequireStatus(bridge.ConfigureStreamReceiveCredit(
                std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive-credit configuration should enforce 62 bits");
        Require(bridge.ConfigureStreamReceiveCredit(100).IsOk()
                && bridge.ConfigureStreamReceiveCredit(100).IsOk(),
            "HTTP/3 stream receive-credit configuration should be idempotent");
        RequireStatus(bridge.ConfigureStreamReceiveCredit(101),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 stream receive-credit configuration should reject a conflict");
        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && adapter.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, { 'x' } }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, headers.Value() }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, data.Value() }).IsOk(),
            "HTTP/3 stream receive-credit bridge fixture should receive a body");
        const auto noDrain = bridge.RefreshStreamReceiveCredit(adapter);
        Require(noDrain.IsOk() && !noDrain.Value() && actions.actions.empty(),
            "HTTP/3 stream receive credit should not advance before a body pull");

        const auto firstPull = sink.Pull(2);
        const auto firstTarget = bridge.Snapshot();
        Require(firstPull.IsOk()
                && firstPull.Value()
                    == std::vector<std::uint8_t>({ 'a', 'b' })
                && sink.PulledBytes() == 2
                && firstTarget.bodyPulledBytes == 2
                && firstTarget.bodyPulledSinceAttach == 2
                && firstTarget.streamReceiveCreditConfigured
                && firstTarget.streamReceiveCreditBaseLimit == 100
                && firstTarget.streamReceiveCreditTargetLimit == 102
                && firstTarget.streamReceiveCreditTargetValid,
            "HTTP/3 stream receive-credit snapshot should derive the first target");
        const auto firstRefresh = bridge.RefreshStreamReceiveCredit(adapter);
        Require(firstRefresh.IsOk() && firstRefresh.Value()
                && actions.actions.size() == 1
                && actions.actions.back().kind
                    == Http3QuicActionKind::StreamReceiveCredit
                && actions.actions.back().streamId == 8
                && actions.actions.back().flowCredit == 102,
            "HTTP/3 stream receive-credit refresh should submit the pulled delta");
        const auto firstActionId = actions.actions.back().actionId;

        Require(sink.Pull(8).Value()
                    == std::vector<std::uint8_t>({ 'c', 'd' })
                && bridge.Snapshot().streamReceiveCreditTargetLimit == 104,
            "HTTP/3 stream receive-credit target should include later pending pulls");
        const auto whilePending = bridge.RefreshStreamReceiveCredit(adapter);
        Require(whilePending.IsOk() && !whilePending.Value()
                && actions.actions.size() == 1,
            "HTTP/3 stream receive credit should wait for pending feedback");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    firstActionId, 0, 0x71 }).IsOk(),
            "HTTP/3 stream receive-credit fixture should reject the first target");
        const auto retryRefresh = bridge.RefreshStreamReceiveCredit(adapter);
        Require(retryRefresh.IsOk() && retryRefresh.Value()
                && actions.actions.size() == 2
                && actions.actions.back().flowCredit == 104,
            "HTTP/3 rejected stream receive credit should retry the latest target");
        const auto retryActionId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    retryActionId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(8).Value().streamCredit == 104,
            "HTTP/3 stream receive-credit retry should commit after acceptance");
        const auto covered = bridge.RefreshStreamReceiveCredit(adapter);
        Require(covered.IsOk() && !covered.Value(),
            "HTTP/3 stream receive credit should not repeat a covered target");

        Require(bridge.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, laterData.Value() }).IsOk()
                && sink.Pull(1).Value()
                    == std::vector<std::uint8_t>({ 'e' })
                && bridge.Snapshot().streamReceiveCreditTargetLimit == 105
                && adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Blocked }).IsOk(),
            "HTTP/3 stream receive-credit fixture should observe another pull");
        RequireStatus(bridge.RefreshStreamReceiveCredit(adapter),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 stream receive-credit refresh should preserve a blocked target");
        Require(actions.actions.size() == 2
                && bridge.Snapshot().streamReceiveCreditTargetLimit == 105
                && adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable }).IsOk(),
            "HTTP/3 blocked refresh should emit no action and preserve coordinates");
        const auto laterRefresh = bridge.RefreshStreamReceiveCredit(adapter);
        Require(laterRefresh.IsOk() && laterRefresh.Value()
                && actions.actions.back().flowCredit == 105,
            "HTTP/3 stream receive credit should submit after transport resumes");
        const auto laterActionId = actions.actions.back().actionId;
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    laterActionId, 0, 0 }).IsOk()
                && sink.Pull(8).Value()
                    == std::vector<std::uint8_t>({ 'f' }),
            "HTTP/3 stream receive-credit fixture should drain its remaining body");
        const auto finalRefresh = bridge.RefreshStreamReceiveCredit(adapter);
        Require(finalRefresh.IsOk() && finalRefresh.Value()
                && actions.actions.back().flowCredit == 106,
            "HTTP/3 stream receive credit should include the final pulled byte");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    actions.actions.back().actionId, 0, 0 }).IsOk(),
            "HTTP/3 stream receive-credit fixture should accept the final target");

        bridge.Reset();
        const auto reset = bridge.Snapshot();
        Require(!reset.streamReceiveCreditConfigured
                && reset.streamReceiveCreditBaseLimit == 0
                && reset.bodyPulledBytes == 6
                && reset.bodyPulledSinceAttach == 0
                && !reset.streamReceiveCreditTargetValid,
            "HTTP/3 request bridge Reset should rebaseline pulled bytes");
        RequireStatus(bridge.RefreshStreamReceiveCredit(adapter),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 request bridge Reset should require receive-credit reconfiguration");
        Require(bridge.ConfigureStreamReceiveCredit(200).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, headers.Value() }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamData,
                    8, 0, data.Value() }).IsOk()
                && sink.Pull(1).Value()
                    == std::vector<std::uint8_t>({ 'a' }),
            "HTTP/3 moving stream receive-credit bridge should derive a new target");
        Http3QuicRequestStreamBridge live(std::move(bridge));
        RequireStatus(bridge.RefreshStreamReceiveCredit(adapter),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from request bridge should reject receive-credit refresh");
        Require(live.Snapshot().streamReceiveCreditTargetLimit == 201,
            "HTTP/3 request bridge move should transfer receive-credit coordinates");
        const auto movedRefresh = live.RefreshStreamReceiveCredit(adapter);
        Require(movedRefresh.IsOk() && movedRefresh.Value()
                && actions.actions.back().flowCredit == 201,
            "HTTP/3 moved request bridge should submit its live target");

        HttpBodySink overflowSink({ 2, 0, 2 });
        Http3QuicRequestStreamBridge overflowBridge(12);
        Http3QuicAdapter overflowAdapter(&actions);
        Require(overflowBridge.AttachBodySink(&overflowSink).IsOk()
                && overflowBridge.ConfigureStreamReceiveCredit(
                    (std::uint64_t{ 1 } << 62) - 1).IsOk()
                && overflowAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && overflowAdapter.Feed({ Http3QuicEventKind::StreamData,
                    12, 0, { 'x' } }).IsOk()
                && overflowBridge.Feed({ Http3QuicEventKind::StreamData,
                    12, 0, headers.Value() }).IsOk()
                && overflowBridge.Feed({ Http3QuicEventKind::StreamData,
                    12, 0, laterData.Value() }).IsOk()
                && overflowSink.Pull(1).IsOk()
                && !overflowBridge.Snapshot().streamReceiveCreditTargetValid,
            "HTTP/3 stream receive-credit overflow fixture should invalidate its target");
        RequireStatus(overflowBridge.RefreshStreamReceiveCredit(overflowAdapter),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 stream receive-credit refresh should reject target overflow");
    }

    void TestHttp3ConnectionReceiveCreditAggregation() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;

        constexpr auto MaxQuicVarInt = (std::uint64_t{ 1 } << 62) - 1;
        Http3ConnectionReceiveCreditCoordinator coordinator;
        Http3QuicRequestStreamBridge missingSink(16);
        RequireStatus(coordinator.RegisterRequestStream(missingSink.Snapshot()),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit registration should require configuration");
        RequireStatus(coordinator.Configure(0),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive-credit configuration should reject zero");
        RequireStatus(coordinator.Configure(std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive-credit configuration should enforce 62 bits");
        Require(coordinator.Configure(100).IsOk()
                && coordinator.Configure(100).IsOk(),
            "HTTP/3 connection receive-credit configuration should be idempotent");
        RequireStatus(coordinator.Configure(101),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit configuration should reject a conflict");
        RequireStatus(coordinator.RegisterRequestStream(missingSink.Snapshot()),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit registration should require a body sink");

        HttpBodySink firstSink({ 8, 0, 8 });
        HttpBodySink secondSink({ 8, 0, 8 });
        Http3QuicRequestStreamBridge firstBridge(8);
        Http3QuicRequestStreamBridge secondBridge(12);
        Require(firstBridge.AttachBodySink(&firstSink).IsOk()
                && secondBridge.AttachBodySink(&secondSink).IsOk()
                && coordinator.RegisterRequestStream(firstBridge.Snapshot()).IsOk()
                && coordinator.RegisterRequestStream(secondBridge.Snapshot()).IsOk()
                && coordinator.RegisterRequestStream(firstBridge.Snapshot()).IsOk(),
            "HTTP/3 connection receive-credit coordinator should register two streams");

        Http3QuicAdapter adapter(&actions);
        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 connection receive-credit aggregate adapter should become ready");
        const auto noDrain = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(noDrain.IsOk() && !noDrain.Value() && actions.actions.empty(),
            "HTTP/3 connection receive credit should not advance without drain");

        const std::vector<std::uint8_t> firstBytes{ 'a', 'b' };
        const std::vector<std::uint8_t> secondBytes{ 'c', 'd', 'e' };
        Require(firstSink.Push(firstBytes.data(), firstBytes.size()).IsOk()
                && secondSink.Push(secondBytes.data(), secondBytes.size()).IsOk()
                && firstSink.Pull(1).Value()
                    == std::vector<std::uint8_t>({ 'a' })
                && secondSink.Pull(2).Value()
                    == std::vector<std::uint8_t>({ 'c', 'd' }),
            "HTTP/3 connection receive-credit aggregate fixture should drain both streams");
        const auto firstObserved = coordinator.ObserveRequestStream(
            firstBridge.Snapshot());
        const auto firstUnchanged = coordinator.ObserveRequestStream(
            firstBridge.Snapshot());
        const auto secondObserved = coordinator.ObserveRequestStream(
            secondBridge.Snapshot());
        const auto firstTarget = coordinator.Snapshot();
        Require(firstObserved.IsOk() && firstObserved.Value()
                && firstUnchanged.IsOk() && !firstUnchanged.Value()
                && secondObserved.IsOk() && secondObserved.Value()
                && firstTarget.configured && firstTarget.initialLimit == 100
                && firstTarget.registeredStreams == 2
                && firstTarget.aggregatePulledBytes == 3
                && firstTarget.targetLimit == 103 && firstTarget.targetValid,
            "HTTP/3 connection receive-credit coordinator should aggregate both streams");

        const auto firstRefresh = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(firstRefresh.IsOk() && firstRefresh.Value()
                && actions.actions.size() == 1
                && actions.actions.back().kind
                    == Http3QuicActionKind::ConnectionReceiveCredit
                && actions.actions.back().streamId == 0
                && actions.actions.back().flowCredit == 103,
            "HTTP/3 connection receive-credit coordinator should submit aggregate target");
        const auto firstActionId = actions.actions.back().actionId;

        Require(firstSink.Pull(8).Value()
                    == std::vector<std::uint8_t>({ 'b' })
                && secondSink.Pull(8).Value()
                    == std::vector<std::uint8_t>({ 'e' })
                && coordinator.ObserveRequestStream(firstBridge.Snapshot()).Value()
                && coordinator.ObserveRequestStream(secondBridge.Snapshot()).Value()
                && coordinator.Snapshot().aggregatePulledBytes == 5,
            "HTTP/3 connection receive-credit pending target should retain later drains");
        const auto whilePending = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(whilePending.IsOk() && !whilePending.Value()
                && actions.actions.size() == 1,
            "HTTP/3 connection receive credit should wait for pending feedback");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    firstActionId, 0, 0x71 }).IsOk(),
            "HTTP/3 connection receive-credit fixture should reject the first target");
        const auto retry = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(retry.IsOk() && retry.Value() && actions.actions.size() == 2
                && actions.actions.back().flowCredit == 105,
            "HTTP/3 rejected connection receive credit should retry the latest target");
        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    actions.actions.back().actionId, 0, 0 }).IsOk()
                && adapter.ReceiveCredit(0).Value().connectionCredit == 105,
            "HTTP/3 connection receive-credit retry should commit after acceptance");
        const auto covered = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(covered.IsOk() && !covered.Value(),
            "HTTP/3 connection receive credit should not repeat a covered target");

        auto conflicting = firstBridge.Snapshot();
        ++conflicting.bodyPulledBytes;
        RequireStatus(coordinator.RegisterRequestStream(conflicting),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit registration should reject a conflict");
        auto regressed = firstBridge.Snapshot();
        --regressed.bodyPulledBytes;
        RequireStatus(coordinator.ObserveRequestStream(regressed),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit observation should reject regression");
        HttpBodySink unknownSink({ 1, 0, 1 });
        Http3QuicRequestStreamBridge unknownBridge(20);
        Require(unknownBridge.AttachBodySink(&unknownSink).IsOk(),
            "HTTP/3 connection receive-credit unknown stream fixture should attach");
        RequireStatus(coordinator.ObserveRequestStream(unknownBridge.Snapshot()),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit observation should require registration");
        Require(coordinator.Snapshot().aggregatePulledBytes == 5,
            "HTTP/3 invalid aggregate observations should preserve the ledger");

        Require(coordinator.UnregisterRequestStream(8).IsOk()
                && coordinator.Snapshot().registeredStreams == 1
                && coordinator.Snapshot().aggregatePulledBytes == 5,
            "HTTP/3 connection receive-credit unregister should preserve consumed bytes");
        RequireStatus(coordinator.UnregisterRequestStream(8),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit unregister should reject an unknown stream");
        RequireStatus(coordinator.ObserveRequestStream(firstBridge.Snapshot()),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 connection receive-credit unregistered stream should not observe");
        Require(coordinator.RegisterRequestStream(firstBridge.Snapshot()).IsOk()
                && firstSink.Push(firstBytes.data(), 1).IsOk()
                && firstSink.Pull(1).IsOk()
                && coordinator.ObserveRequestStream(firstBridge.Snapshot()).Value()
                && coordinator.Snapshot().aggregatePulledBytes == 6,
            "HTTP/3 re-registration should baseline current bytes and count later drain");

        Require(adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Blocked }).IsOk(),
            "HTTP/3 aggregate adapter should enter blocked state");
        RequireStatus(coordinator.RefreshConnectionReceiveCredit(adapter),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 blocked aggregate refresh should preserve its target");
        Require(actions.actions.size() == 2
                && coordinator.Snapshot().targetLimit == 106
                && adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable }).IsOk(),
            "HTTP/3 blocked aggregate refresh should emit no action");
        const auto laterRefresh = coordinator.RefreshConnectionReceiveCredit(adapter);
        Require(laterRefresh.IsOk() && laterRefresh.Value()
                && actions.actions.back().flowCredit == 106
                && adapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    actions.actions.back().actionId, 0, 0 }).IsOk(),
            "HTTP/3 aggregate refresh should submit after transport resumes");

        Http3ConnectionReceiveCreditCoordinator live(std::move(coordinator));
        RequireStatus(coordinator.RefreshConnectionReceiveCredit(adapter),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 moved-from aggregate coordinator should reject refresh");
        Require(live.Snapshot().registeredStreams == 2
                && live.Snapshot().aggregatePulledBytes == 6
                && live.Snapshot().targetLimit == 106,
            "HTTP/3 aggregate coordinator move should transfer its ledger");
        live.Reset();
        Require(!live.Snapshot().configured
                && live.Snapshot().registeredStreams == 0
                && live.Snapshot().aggregatePulledBytes == 0
                && !live.Snapshot().targetValid,
            "HTTP/3 aggregate coordinator Reset should clear its ledger");
        RequireStatus(live.RefreshConnectionReceiveCredit(adapter),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 reset aggregate coordinator should require configuration");

        Http3ConnectionReceiveCreditCoordinator overflow;
        HttpBodySink overflowSink({ 2, 0, 2 });
        Http3QuicRequestStreamBridge overflowBridge(24);
        const std::vector<std::uint8_t> overflowBytes{ 'x', 'y' };
        Require(overflow.Configure(MaxQuicVarInt - 1).IsOk()
                && overflowBridge.AttachBodySink(&overflowSink).IsOk()
                && overflow.RegisterRequestStream(overflowBridge.Snapshot()).IsOk()
                && overflowSink.Push(
                    overflowBytes.data(), overflowBytes.size()).IsOk()
                && overflowSink.Pull(1).IsOk()
                && overflow.ObserveRequestStream(overflowBridge.Snapshot()).Value()
                && overflow.Snapshot().targetLimit == MaxQuicVarInt
                && overflowSink.Pull(1).IsOk(),
            "HTTP/3 connection receive-credit overflow fixture should reach the limit");
        RequireStatus(overflow.ObserveRequestStream(overflowBridge.Snapshot()),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 connection receive-credit aggregation should reject overflow");
        Require(overflow.Snapshot().aggregatePulledBytes == 1
                && overflow.Snapshot().targetLimit == MaxQuicVarInt,
            "HTTP/3 aggregate overflow should preserve the prior ledger");
    }

    void TestHttp3QuicBodyBudgetBinding() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;

        HttpBodyBudget budget({ 8, 4 });
        HttpBodyBudget otherBudget({ 8, 4 });
        Http3QuicAdapter adapter(&actions);
        RequireStatus(adapter.AttachBodyBudget(nullptr),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject a null body budget");
        Require(adapter.AttachBodyBudget(&budget).IsOk()
                && adapter.AttachBodyBudget(&budget).IsOk()
                && adapter.HasBodyBudget(),
            "HTTP/3 QUIC adapter should attach one body budget idempotently");
        RequireStatus(adapter.AttachBodyBudget(&otherBudget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject a different body budget");
        Require(adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && adapter.SendStreamData(4, { 'a', 'b', 'c' }).IsOk()
                && budget.ReservedBytes() == 3
                && budget.ReservedBytes(4) == 3
                && adapter.Snapshot().pendingDataActions == 1
                && adapter.Snapshot().pendingDataBytes == 3
                && adapter.Snapshot().pendingBodyBudgetBytes == 3,
            "HTTP/3 QUIC adapter should reserve pending DATA bytes");
        const auto pending = adapter.PendingData(4);
        Require(pending.IsOk()
                && pending.Value().actions == 1
                && pending.Value().bytes == 3
                && pending.Value().bodyBudgetBytes == 3,
            "HTTP/3 QUIC adapter should expose per-stream pending DATA bytes");
        RequireStatus(adapter.PendingData(std::uint64_t{ 1 } << 62),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject an invalid pending-DATA stream id");
        const auto rejectedActionId = actions.actions.back().actionId;
        RequireStatus(adapter.FeedTransportFeedback({
                Http3QuicTransportFeedbackKind::ActionAccepted,
                rejectedActionId, 2, 0 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC adapter should reject partial DATA acceptance");
        Require(budget.ReservedBytes(4) == 3
                && adapter.Snapshot().pendingDataActions == 1
                && adapter.Snapshot().pendingDataBytes == 3
                && adapter.PendingData(4).Value().bodyBudgetBytes == 3,
            "HTTP/3 QUIC adapter should retain budget after invalid feedback");
        auto rejected = adapter.FeedTransportFeedbackWithRejectedData({
            Http3QuicTransportFeedbackKind::ActionRejected,
            rejectedActionId, 0, 0x70 });
        Require(rejected.IsOk()
                && rejected.Value().available
                && rejected.Value().actionId == rejectedActionId
                && rejected.Value().streamId == 4
                && rejected.Value().payload
                    == std::vector<std::uint8_t>({ 'a', 'b', 'c' })
                && budget.ReservedBytes() == 0
                && adapter.Snapshot().pendingDataActions == 0
                && adapter.Snapshot().pendingDataBytes == 0
                && adapter.Snapshot().pendingBodyBudgetBytes == 0,
            "HTTP/3 QUIC adapter should hand off rejected DATA and release budget");
        RequireStatus(adapter.FeedTransportFeedbackWithRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                rejectedActionId, 0, 0x70 }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should hand off rejected DATA only once");
        Require(adapter.SendStreamData(4, { 'd', 'e' }).IsOk()
                && budget.ReservedBytes(4) == 2,
            "HTTP/3 QUIC adapter should reserve a second DATA action");
        const auto acceptedActionId = actions.actions.back().actionId;
        auto acceptedFeedback = adapter.FeedTransportFeedbackWithRejectedData({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            acceptedActionId, 2, 0 });
        Require(acceptedFeedback.IsOk()
                && !acceptedFeedback.Value().available
                && acceptedFeedback.Value().payload.empty()
                && budget.ReservedBytes() == 0,
            "HTTP/3 QUIC adapter should release budget after DATA acceptance");
        Require(adapter.SendStreamData(4, { 'f' }).IsOk()
                && budget.ReservedBytes(4) == 1,
            "HTTP/3 QUIC adapter reset fixture should hold one reservation");
        adapter.Reset();
        Require(budget.ReservedBytes() == 0
                && adapter.HasBodyBudget()
                && adapter.Snapshot().pendingDataActions == 0,
            "HTTP/3 QUIC adapter Reset should release pending DATA budget");

        HttpBodyBudget strictBudget({ 1, 1 });
        ActionRecorder strictActions;
        Http3QuicAdapter strictAdapter(&strictActions);
        Require(strictAdapter.AttachBodyBudget(&strictBudget).IsOk()
                && strictAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC strict-budget fixture should become ready");
        RequireStatus(strictAdapter.SendStreamData(8, { 'a', 'b' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should require a full body-budget reservation");
        Require(strictBudget.ReservedBytes() == 0
                && strictActions.actions.empty()
                && strictAdapter.Snapshot().activeStreams == 0
                && strictAdapter.Snapshot().pendingDataActions == 0,
            "HTTP/3 QUIC partial budget rejection should preserve adapter state");

        HttpBodyBudget noSinkBudget({ 4, 4 });
        Http3QuicAdapter noSinkAdapter;
        Require(noSinkAdapter.AttachBodyBudget(&noSinkBudget).IsOk()
                && noSinkAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 QUIC no-sink budget fixture should become ready");
        RequireStatus(noSinkAdapter.SendStreamData(12, { 'x' }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject DATA without an action sink");
        Require(noSinkBudget.ReservedBytes() == 0
                && noSinkAdapter.Snapshot().activeStreams == 0
                && noSinkAdapter.Snapshot().pendingDataActions == 0,
            "HTTP/3 QUIC sink failure should roll back body budget and stream state");

        HttpBodyBudget actionLimitBudget({ 8, 8 });
        ActionRecorder actionLimitActions;
        Http3QuicAdapter actionLimitAdapter(&actionLimitActions);
        Require(actionLimitAdapter.AttachBodyBudget(&actionLimitBudget).IsOk()
                && actionLimitAdapter.SetLimits({ 2, 4, 1, 8 }).IsOk()
                && actionLimitAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && actionLimitAdapter.SendStreamData(16, { 'a' }).IsOk(),
            "HTTP/3 QUIC action-limit budget fixture should submit once");
        RequireStatus(actionLimitAdapter.SendStreamData(16, { 'b' }),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 QUIC adapter should enforce its action limit");
        Require(actionLimitBudget.ReservedBytes(16) == 1
                && actionLimitActions.actions.size() == 1
                && actionLimitAdapter.Snapshot().pendingDataActions == 1
                && actionLimitAdapter.Snapshot().pendingDataBytes == 1,
            "HTTP/3 QUIC action-limit failure should roll back only new budget bytes");
        actionLimitAdapter.Reset();
        Require(actionLimitBudget.ReservedBytes() == 0,
            "HTTP/3 QUIC action-limit Reset should release the retained reservation");

        HttpBodyBudget lateBudget({ 4, 4 });
        ActionRecorder lateActions;
        Http3QuicAdapter lateAdapter(&lateActions);
        Require(lateAdapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && lateAdapter.SendStreamData(20, { 'l' }).IsOk(),
            "HTTP/3 QUIC late-attach fixture should create unbound pending DATA");
        RequireStatus(lateAdapter.AttachBodyBudget(&lateBudget),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC adapter should reject body-budget attachment with pending DATA");
        Require(lateBudget.ReservedBytes() == 0
                && lateAdapter.Snapshot().pendingDataBytes == 1
                && lateAdapter.Snapshot().pendingBodyBudgetBytes == 0
                && lateAdapter.PendingData(20).Value().bodyBudgetBytes == 0
                && lateAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    lateActions.actions.back().actionId, 1, 0 }).IsOk(),
            "HTTP/3 QUIC late-attach rejection should preserve unattached behavior");

        HttpBodyBudget movedFromBudget({ 4, 4 });
        HttpBodyBudget liveBudget(std::move(movedFromBudget));
        Http3QuicAdapter movedBudgetAdapter;
        RequireStatus(movedBudgetAdapter.AttachBodyBudget(&movedFromBudget),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 QUIC adapter should reject a moved-from body budget");
        Require(liveBudget.LastError().IsOk(),
            "HTTP/3 QUIC moved-budget fixture should preserve the moved-to budget");
        Http3QuicAdapter movedPendingAdapter(std::move(movedBudgetAdapter));
        RequireStatus(movedBudgetAdapter.PendingData(0),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 QUIC moved-from adapter should reject pending-DATA queries");
        Require(movedPendingAdapter.PendingData(0).IsOk(),
            "HTTP/3 QUIC moved-to adapter should support pending-DATA queries");

        HttpBodyBudget replacedBudget({ 8, 8 });
        HttpBodyBudget incomingBudget({ 8, 8 });
        ActionRecorder replacedActions;
        ActionRecorder incomingActions;
        Http3QuicAdapter assignedAdapter(&replacedActions);
        Http3QuicAdapter incomingAdapter(&incomingActions);
        Require(assignedAdapter.AttachBodyBudget(&replacedBudget).IsOk()
                && incomingAdapter.AttachBodyBudget(&incomingBudget).IsOk()
                && assignedAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && incomingAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && assignedAdapter.SendStreamData(24, { 'o' }).IsOk()
                && incomingAdapter.SendStreamData(28, { 'n', 'e' }).IsOk(),
            "HTTP/3 QUIC move-assignment fixture should hold both reservations");
        const auto incomingActionId = incomingActions.actions.back().actionId;
        assignedAdapter = std::move(incomingAdapter);
        Require(replacedBudget.ReservedBytes() == 0
                && incomingBudget.ReservedBytes(28) == 2
                && assignedAdapter.HasBodyBudget()
                && !incomingAdapter.HasBodyBudget(),
            "HTTP/3 QUIC move assignment should release replaced and retain moved budget");
        Require(assignedAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    incomingActionId, 2, 0 }).IsOk()
                && incomingBudget.ReservedBytes() == 0,
            "HTTP/3 QUIC moved adapter should release its retained reservation");

        HttpBodyBudget lifetimeBudget({ 4, 4 });
        ActionRecorder lifetimeActions;
        {
            Http3QuicAdapter lifetimeAdapter(&lifetimeActions);
            Require(lifetimeAdapter.AttachBodyBudget(&lifetimeBudget).IsOk()
                    && lifetimeAdapter.Feed({
                        Http3QuicEventKind::HandshakeComplete }).IsOk()
                    && lifetimeAdapter.SendStreamData(32, { 'z' }).IsOk()
                    && lifetimeBudget.ReservedBytes(32) == 1,
                "HTTP/3 QUIC lifetime fixture should hold pending DATA");
        }
        Require(lifetimeBudget.ReservedBytes() == 0,
            "HTTP/3 QUIC adapter destruction should release pending DATA budget");
    }

    void TestHttp3QuicPreparedProducerTransfer() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;
        struct BudgetObserver final : HttpBodyBudgetObserver {
            int pauseConnectionCount = 0;
            int resumeConnectionCount = 0;
            int pauseStreamCount = 0;
            int resumeStreamCount = 0;

            void PauseConnection() noexcept override { ++pauseConnectionCount; }
            void ResumeConnection() noexcept override { ++resumeConnectionCount; }
            void PauseStream(std::uint64_t) noexcept override { ++pauseStreamCount; }
            void ResumeStream(std::uint64_t) noexcept override { ++resumeStreamCount; }
        } budgetObserver;

        const std::vector<std::uint8_t> source{ 'p', 'a', 'y' };
        HttpBodyBudget budget({ 8, 8 }, { 1, 2, 1, 2 });
        HttpBodyProducer producer({ 8, 1, 8 });
        Http3QuicAdapter adapter(&actions);
        Require(budget.AttachObserver(&budgetObserver).IsOk()
                && producer.AttachBudget(&budget, 4).IsOk()
                && adapter.AttachBodyBudget(&budget).IsOk()
                && adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && producer.Push(source.data(), source.size()).IsOk(),
            "HTTP/3 prepared-producer transfer fixture should reserve source bytes");
        auto prepared = producer.PreparePull(source.size());
        Require(prepared.IsOk() && prepared.Value().available,
            "HTTP/3 prepared-producer transfer fixture should prepare source bytes");
        prepared.Value().payload.front() = 'x';
        Require(adapter.SendPreparedStreamData(
                    4, producer, prepared.Value().id).IsOk()
                && !producer.HasPreparedPull()
                && producer.BufferedBytes() == 0
                && budget.ReservedBytes(4) == source.size()
                && adapter.Snapshot().pendingDataActions == 1
                && adapter.Snapshot().pendingDataBytes == source.size()
                && adapter.Snapshot().pendingBodyBudgetBytes == source.size()
                && actions.actions.size() == 1
                && actions.actions.back().payload == source
                && budgetObserver.pauseConnectionCount == 1
                && budgetObserver.pauseStreamCount == 1
                && budgetObserver.resumeConnectionCount == 0
                && budgetObserver.resumeStreamCount == 0,
            "HTTP/3 prepared-producer send should copy the queue prefix and transfer budget without a release gap");
        RequireStatus(adapter.SendPreparedStreamData(
                4, producer, prepared.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 prepared-producer send should consume an id only once");
        auto rejected = adapter.FeedTransportFeedbackWithRejectedData({
            Http3QuicTransportFeedbackKind::ActionRejected,
            actions.actions.back().actionId, 0, 0x71 });
        Require(rejected.IsOk()
                && rejected.Value().available
                && rejected.Value().payload == source
                && budget.ReservedBytes() == 0
                && budgetObserver.resumeConnectionCount == 1
                && budgetObserver.resumeStreamCount == 1,
            "HTTP/3 rejected transferred DATA should hand off bytes and release the inherited reservation");

        HttpBodyBudget retainedBudget({ 8, 8 });
        HttpBodyBudget wrongBudget({ 8, 8 });
        HttpBodyProducer retainedProducer({ 8, 1, 8 });
        Http3QuicAdapter wrongBudgetAdapter(&actions);
        Http3QuicAdapter noSinkAdapter;
        Require(retainedProducer.AttachBudget(&retainedBudget, 8).IsOk()
                && retainedProducer.Push(source.data(), 2).IsOk(),
            "HTTP/3 prepared-producer failure fixture should reserve bytes");
        auto retained = retainedProducer.PreparePull(2);
        Require(retained.IsOk()
                && wrongBudgetAdapter.AttachBodyBudget(&wrongBudget).IsOk()
                && wrongBudgetAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && noSinkAdapter.AttachBodyBudget(&retainedBudget).IsOk()
                && noSinkAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk(),
            "HTTP/3 prepared-producer failure adapters should become ready");
        RequireStatus(wrongBudgetAdapter.SendPreparedStreamData(
                8, retainedProducer, retained.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 prepared-producer send should reject a different budget");
        RequireStatus(noSinkAdapter.SendPreparedStreamData(
                12, retainedProducer, retained.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 prepared-producer send should reject a different budget stream");
        RequireStatus(noSinkAdapter.SendPreparedStreamData(
                8, retainedProducer, retained.Value().id),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 prepared-producer send should retain state without an action sink");
        Require(retainedProducer.HasPreparedPull()
                && retainedProducer.PreparedPullBytes() == 2
                && retainedProducer.BufferedBytes() == 2
                && retainedBudget.ReservedBytes(8) == 2
                && noSinkAdapter.Snapshot().actionCount == 0
                && noSinkAdapter.Snapshot().activeStreams == 0
                && noSinkAdapter.Snapshot().pendingDataActions == 0,
            "HTTP/3 prepared-producer local failures should preserve producer and adapter ledgers");
        retainedProducer.Cancel();
        Require(retainedBudget.ReservedBytes() == 0,
            "HTTP/3 prepared-producer failure cleanup should release retained bytes");

        HttpBodyBudget adapterBudget({ 8, 8 });
        HttpBodyProducer unbudgetedProducer({ 8, 1, 8 });
        ActionRecorder unbudgetedActions;
        Http3QuicAdapter unbudgetedAdapter(&unbudgetedActions);
        Require(unbudgetedProducer.Push(source.data(), 2).IsOk(),
            "HTTP/3 unbudgeted prepared-producer fixture should buffer bytes");
        auto unbudgeted = unbudgetedProducer.PreparePull(2);
        Require(unbudgeted.IsOk()
                && unbudgetedAdapter.AttachBodyBudget(&adapterBudget).IsOk()
                && unbudgetedAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && unbudgetedAdapter.SendPreparedStreamData(
                    16, unbudgetedProducer, unbudgeted.Value().id).IsOk()
                && unbudgetedProducer.BufferedBytes() == 0
                && !unbudgetedProducer.HasPreparedPull()
                && adapterBudget.ReservedBytes(16) == 2,
            "HTTP/3 unbudgeted producer should use the adapter's normal reservation path");
        Require(unbudgetedAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    unbudgetedActions.actions.back().actionId, 2, 0 }).IsOk()
                && adapterBudget.ReservedBytes() == 0,
            "HTTP/3 accepted unbudgeted producer DATA should release adapter-owned budget");
    }

    void TestHttp3QuicRejectedDataProducerRequeue() {
        using namespace LikesProgram::Http;

        struct ActionRecorder final : Http3QuicActionSink {
            std::vector<Http3QuicAction> actions;

            void Submit(const Http3QuicAction& action) noexcept override {
                actions.push_back(action);
            }
        } actions;
        struct BudgetObserver final : HttpBodyBudgetObserver {
            int pauseConnectionCount = 0;
            int resumeConnectionCount = 0;
            int pauseStreamCount = 0;
            int resumeStreamCount = 0;

            void PauseConnection() noexcept override { ++pauseConnectionCount; }
            void ResumeConnection() noexcept override { ++resumeConnectionCount; }
            void PauseStream(std::uint64_t) noexcept override { ++pauseStreamCount; }
            void ResumeStream(std::uint64_t) noexcept override { ++resumeStreamCount; }
        } budgetObserver;

        const std::vector<std::uint8_t> rejectedBytes{ 'p', 'a', 'y' };
        const std::vector<std::uint8_t> laterBytes{ 'x', 'y' };
        HttpBodyBudget budget({ 16, 16 }, { 1, 2, 1, 2 });
        HttpBodyProducer producer({ 8, 1, 8 });
        Http3QuicAdapter adapter(&actions);
        Require(budget.AttachObserver(&budgetObserver).IsOk()
                && producer.AttachBudget(&budget, 4).IsOk()
                && adapter.AttachBodyBudget(&budget).IsOk()
                && adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()
                && adapter.Feed({ Http3QuicEventKind::ConnectionSendCredit,
                    0, 0, {}, 8 }).IsOk()
                && adapter.Feed({ Http3QuicEventKind::StreamSendCredit,
                    4, 0, {}, 8 }).IsOk()
                && producer.Push(
                    rejectedBytes.data(), rejectedBytes.size()).Value()
                    == rejectedBytes.size(),
            "HTTP/3 rejected-DATA producer requeue fixture should reserve source bytes");
        auto prepared = producer.PreparePull(rejectedBytes.size());
        Require(prepared.IsOk()
                && adapter.SendPreparedStreamData(
                    4, producer, prepared.Value().id).IsOk()
                && producer.Push(laterBytes.data(), laterBytes.size()).Value()
                    == laterBytes.size(),
            "HTTP/3 rejected-DATA producer requeue fixture should hold pending and later bytes");
        const auto actionId = actions.actions.back().actionId;
        auto laterPrepared = producer.PreparePull(laterBytes.size());
        const auto creditAfterSend = adapter.SendCredit(4);
        Require(laterPrepared.IsOk()
                && creditAfterSend.IsOk()
                && creditAfterSend.Value().availableCredit == 5,
            "HTTP/3 rejected-DATA producer requeue fixture should prepare later bytes");
        RequireStatus(adapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                actionId, 0, 0x71 }, producer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should reject an active prepared pull");
        Require(producer.BufferedBytes() == laterBytes.size()
                && producer.HasPreparedPull()
                && adapter.Snapshot().pendingDataActions == 1
                && adapter.Snapshot().pendingDataBytes == rejectedBytes.size()
                && budget.ReservedBytes(4)
                    == rejectedBytes.size() + laterBytes.size()
                && adapter.SendCredit(4).Value().availableCredit == 5
                && !adapter.LastErrorContext().transportFeedbackValid,
            "HTTP/3 rejected-DATA validation failure should preserve every ledger");
        Require(producer.RollbackPull(laterPrepared.Value().id).IsOk()
                && producer.Close().IsOk()
                && adapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    actionId, 0, 0x71 }, producer).IsOk(),
            "HTTP/3 rejected-DATA requeue should restore a closed producer");
        Require(producer.IsClosed()
                && producer.BufferedBytes()
                    == rejectedBytes.size() + laterBytes.size()
                && adapter.Snapshot().pendingDataActions == 0
                && adapter.Snapshot().pendingDataBytes == 0
                && adapter.Snapshot().pendingBodyBudgetBytes == 0
                && budget.ReservedBytes(4)
                    == rejectedBytes.size() + laterBytes.size()
                && adapter.SendCredit(4).Value().availableCredit == 8
                && adapter.LastErrorContext().transportFeedbackValid
                && adapter.LastErrorContext().transportActionId == actionId
                && budgetObserver.pauseConnectionCount == 1
                && budgetObserver.pauseStreamCount == 1
                && budgetObserver.resumeConnectionCount == 0
                && budgetObserver.resumeStreamCount == 0,
            "HTTP/3 rejected-DATA requeue should transfer shared reservation and restore credit once");
        auto restored = producer.Pull(
            rejectedBytes.size() + laterBytes.size());
        std::vector<std::uint8_t> expected = rejectedBytes;
        expected.insert(expected.end(), laterBytes.begin(), laterBytes.end());
        Require(restored.IsOk()
                && restored.Value() == expected
                && budget.ReservedBytes() == 0
                && budgetObserver.resumeConnectionCount == 1
                && budgetObserver.resumeStreamCount == 1,
            "HTTP/3 rejected-DATA requeue should prepend bytes without a budget release gap");
        RequireStatus(adapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                actionId, 0, 0x71 }, producer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should consume feedback exactly once");

        const std::vector<std::uint8_t> firstOrdered{ 'a', '1' };
        const std::vector<std::uint8_t> secondOrdered{ 'b', '2' };
        const std::vector<std::uint8_t> applicationTail{ 't' };
        ActionRecorder descendingActions;
        HttpBodyBudget descendingBudget({ 16, 16 });
        HttpBodyProducer descendingProducer({ 8, 1, 8 });
        Http3QuicAdapter descendingAdapter(&descendingActions);
        Require(descendingProducer.AttachBudget(&descendingBudget, 32).IsOk()
                && descendingAdapter.AttachBodyBudget(&descendingBudget).IsOk()
                && descendingAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && descendingAdapter.SendStreamData(
                    32, firstOrdered).IsOk()
                && descendingAdapter.SendStreamData(
                    32, secondOrdered).IsOk()
                && descendingProducer.Push(
                    applicationTail.data(), applicationTail.size()).Value()
                    == applicationTail.size(),
            "HTTP/3 ordered requeue fixture should hold two budgeted DATA actions");
        const auto descendingFirstId =
            descendingActions.actions[descendingActions.actions.size() - 2].actionId;
        const auto descendingSecondId = descendingActions.actions.back().actionId;
        Require(descendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    descendingSecondId, 0, 0 }, descendingProducer).IsOk()
                && descendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    descendingFirstId, 0, 0 }, descendingProducer).IsOk()
                && descendingAdapter.Snapshot().pendingDataActions == 0
                && descendingBudget.ReservedBytes(32)
                    == firstOrdered.size() + secondOrdered.size()
                        + applicationTail.size(),
            "HTTP/3 descending rejection feedback should transfer both reservations");
        std::vector<std::uint8_t> orderedPair = firstOrdered;
        orderedPair.insert(
            orderedPair.end(), secondOrdered.begin(), secondOrdered.end());
        const auto descendingReplay = descendingProducer.RejectedDataReplay();
        Require(descendingProducer.HasRejectedDataReplay()
                && descendingProducer.RejectedDataReplayBytes()
                    == orderedPair.size()
                && descendingReplay.available
                && descendingReplay.streamId == 32
                && descendingReplay.lowestActionId == descendingFirstId
                && descendingReplay.bytes == orderedPair.size(),
            "HTTP/3 replay readiness should expose prefix scheduling coordinates");
        auto firstReplay = descendingProducer.PrepareRejectedDataReplay();
        Require(firstReplay.IsOk()
                && firstReplay.Value().available
                && firstReplay.Value().payload == orderedPair,
            "HTTP/3 replay preparation should copy the exact ordered prefix");
        RequireStatus(descendingProducer.PrepareRejectedDataReplay(),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 replay preparation should share the one-outstanding-id guard");
        Require(descendingProducer.RollbackPull(
                    firstReplay.Value().id).IsOk()
                && descendingProducer.RejectedDataReplay().available
                && descendingProducer.RejectedDataReplay().streamId == 32
                && descendingProducer.RejectedDataReplay().lowestActionId
                    == descendingFirstId
                && descendingProducer.RejectedDataReplay().bytes
                    == orderedPair.size(),
            "HTTP/3 replay rollback should preserve prefix scheduling coordinates");
        auto committedReplay = descendingProducer.PrepareRejectedDataReplay();
        Require(committedReplay.IsOk()
                && committedReplay.Value().payload == orderedPair
                && descendingProducer.CommitPull(
                    committedReplay.Value().id).IsOk()
                && !descendingProducer.HasRejectedDataReplay()
                && descendingProducer.RejectedDataReplayBytes() == 0
                && !descendingProducer.RejectedDataReplay().available
                && descendingProducer.RejectedDataReplay().streamId == 0
                && descendingProducer.RejectedDataReplay().lowestActionId == 0
                && descendingProducer.RejectedDataReplay().bytes == 0,
            "HTTP/3 replay commit should consume only the rejected prefix");
        auto unavailableReplay = descendingProducer.PrepareRejectedDataReplay();
        auto tailPrepared = descendingProducer.PreparePull(applicationTail.size());
        Require(unavailableReplay.IsOk()
                && !unavailableReplay.Value().available
                && tailPrepared.IsOk()
                && tailPrepared.Value().payload == applicationTail
                && descendingProducer.CommitPull(tailPrepared.Value().id).IsOk()
                && descendingBudget.ReservedBytes() == 0,
            "HTTP/3 replay preparation should exclude the application tail and preserve legacy prepare");

        ActionRecorder ascendingActions;
        HttpBodyProducer ascendingProducer({ 8, 1, 8 });
        Http3QuicAdapter ascendingAdapter(&ascendingActions);
        Require(ascendingAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && ascendingAdapter.SendStreamData(
                    36, firstOrdered).IsOk()
                && ascendingAdapter.SendStreamData(
                    36, secondOrdered).IsOk(),
            "HTTP/3 ascending requeue fixture should hold two DATA actions");
        const auto ascendingFirstId =
            ascendingActions.actions[ascendingActions.actions.size() - 2].actionId;
        const auto ascendingSecondId = ascendingActions.actions.back().actionId;
        Require(ascendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    ascendingFirstId, 0, 0 }, ascendingProducer).IsOk(),
            "HTTP/3 ascending feedback should requeue the earlier DATA action");
        HttpBodyProducer movedAscending(std::move(ascendingProducer));
        RequireStatus(ascendingProducer.PrepareRejectedDataReplay(),
            LikesProgram::StatusCode::Internal,
            "HTTP/3 rejected-prefix ordering should move with the producer");
        const auto movedReplay = movedAscending.RejectedDataReplay();
        Require(!ascendingProducer.HasRejectedDataReplay()
                && ascendingProducer.RejectedDataReplayBytes() == 0
                && !ascendingProducer.RejectedDataReplay().available
                && movedReplay.available
                && movedReplay.streamId == 36
                && movedReplay.lowestActionId == ascendingFirstId
                && movedReplay.bytes == firstOrdered.size(),
            "HTTP/3 replay coordinates should transfer only to the moved-to producer");
        RequireStatus(ascendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                ascendingSecondId, 0, 0 }, movedAscending),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 ascending feedback should wait while the earlier prefix is buffered");
        const auto blockedReplay = movedAscending.RejectedDataReplay();
        Require(blockedReplay.available
                && blockedReplay.streamId == 36
                && blockedReplay.lowestActionId == ascendingFirstId
                && blockedReplay.bytes == firstOrdered.size(),
            "HTTP/3 order-blocked feedback should preserve replay coordinates");
        auto partialOrdered = movedAscending.Pull(1);
        const auto partialReplay = movedAscending.RejectedDataReplay();
        Require(partialOrdered.IsOk()
                && partialOrdered.Value()
                    == std::vector<std::uint8_t>{ firstOrdered.front() }
                && partialReplay.available
                && partialReplay.streamId == 36
                && partialReplay.lowestActionId == ascendingFirstId
                && partialReplay.bytes == 1,
            "HTTP/3 partial replay consumption should retain coordinates and reduce bytes");
        RequireStatus(ascendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                ascendingSecondId, 0, 0 }, movedAscending),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 ascending feedback should remain pending until the prefix drains");
        auto remainingOrdered = movedAscending.PreparePull(firstOrdered.size());
        Require(remainingOrdered.IsOk()
                && remainingOrdered.Value().available
                && remainingOrdered.Value().payload
                    == std::vector<std::uint8_t>{ firstOrdered.back() }
                && movedAscending.CommitPull(
                    remainingOrdered.Value().id).IsOk()
                && ascendingAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    ascendingSecondId, 0, 0 }, movedAscending).IsOk(),
            "HTTP/3 ascending feedback should requeue after the earlier retry is handed off");
        const auto laterReplay = movedAscending.RejectedDataReplay();
        Require(laterReplay.available
                && laterReplay.streamId == 36
                && laterReplay.lowestActionId == ascendingSecondId
                && laterReplay.bytes == secondOrdered.size(),
            "HTTP/3 later replay should publish its own scheduling coordinates");
        auto ascendingBody = movedAscending.Pull(secondOrdered.size());
        Require(ascendingBody.IsOk()
                && ascendingBody.Value() == secondOrdered
                && ascendingAdapter.Snapshot().pendingDataActions == 0
                && !movedAscending.RejectedDataReplay().available,
            "HTTP/3 ascending feedback should preserve sequential replay order");

        ActionRecorder crossStreamActions;
        HttpBodyProducer crossStreamProducer({ 8, 1, 8 });
        Http3QuicAdapter crossStreamAdapter(&crossStreamActions);
        Require(crossStreamAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && crossStreamAdapter.SendStreamData(
                    40, firstOrdered).IsOk()
                && crossStreamAdapter.SendStreamData(
                    44, secondOrdered).IsOk(),
            "HTTP/3 cross-stream requeue fixture should hold two DATA actions");
        const auto crossFirstId =
            crossStreamActions.actions[crossStreamActions.actions.size() - 2].actionId;
        const auto crossSecondId = crossStreamActions.actions.back().actionId;
        Require(crossStreamAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    crossFirstId, 0, 0 }, crossStreamProducer).IsOk(),
            "HTTP/3 cross-stream fixture should establish one rejected prefix");
        RequireStatus(crossStreamAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                crossSecondId, 0, 0 }, crossStreamProducer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected prefix should reject another stream without mutation");
        const auto crossStreamReplay = crossStreamProducer.RejectedDataReplay();
        Require(crossStreamReplay.available
                && crossStreamReplay.streamId == 40
                && crossStreamReplay.lowestActionId == crossFirstId
                && crossStreamReplay.bytes == firstOrdered.size(),
            "HTTP/3 cross-stream rejection should preserve replay coordinates");
        RequireStatus(crossStreamProducer.SetDeadline(
                LikesProgram::Time::Deadline::At(
                    LikesProgram::Time::Clock::Now())),
            LikesProgram::StatusCode::DeadlineExceeded,
            "HTTP/3 rejected prefix timeout fixture should expire and clear bytes");
        Require(!crossStreamProducer.RejectedDataReplay().available,
            "HTTP/3 timeout should clear replay coordinates");
        crossStreamProducer.Reset();
        Require(crossStreamAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    crossSecondId, 0, 0 }, crossStreamProducer).IsOk()
                && crossStreamProducer.Pull(secondOrdered.size()).Value()
                    == secondOrdered,
            "HTTP/3 timeout and Reset should clear the rejected-prefix ordering guard");

        ActionRecorder retryActions;
        HttpBodyProducer retryProducer({ 8, 1, 8 });
        Http3QuicAdapter retryAdapter(&retryActions);
        Require(retryAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && retryAdapter.SendStreamData(48, firstOrdered).IsOk(),
            "HTTP/3 rejected-prefix retry fixture should submit initial DATA");
        const auto initialRetryId = retryActions.actions.back().actionId;
        Require(retryAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    initialRetryId, 0, 0 }, retryProducer).IsOk(),
            "HTTP/3 rejected-prefix retry fixture should requeue initial DATA");
        auto retryPrepared = retryProducer.PrepareRejectedDataReplay();
        Require(retryPrepared.IsOk()
                && retryAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Blocked }).IsOk(),
            "HTTP/3 replay handoff failure fixture should prepare and block transport");
        RequireStatus(retryAdapter.SendPreparedStreamData(
                48, retryProducer, retryPrepared.Value().id),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 blocked replay handoff should retain the prepared prefix");
        Require(retryProducer.HasPreparedPull()
                && retryProducer.HasRejectedDataReplay()
                && retryProducer.BufferedBytes() == firstOrdered.size()
                && retryProducer.RejectedDataReplay().available
                && retryProducer.RejectedDataReplay().streamId == 48
                && retryProducer.RejectedDataReplay().lowestActionId
                    == initialRetryId
                && retryProducer.RejectedDataReplay().bytes == firstOrdered.size()
                && retryAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::Writable }).IsOk()
                && retryAdapter.SendPreparedStreamData(
                    48, retryProducer, retryPrepared.Value().id).IsOk()
                && retryProducer.BufferedBytes() == 0,
            "HTTP/3 replay handoff retry should consume the prefix only after success");
        const auto repeatedRetryId = retryActions.actions.back().actionId;
        Require(retryAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    repeatedRetryId, 0, 0 }, retryProducer).IsOk()
                && retryAdapter.SendStreamData(52, secondOrdered).IsOk(),
            "HTTP/3 repeated rejection should establish a new replay prefix");
        const auto afterCancelId = retryActions.actions.back().actionId;
        retryProducer.Cancel();
        Require(!retryProducer.RejectedDataReplay().available,
            "HTTP/3 Cancel should clear replay coordinates before Reset");
        retryProducer.Reset();
        Require(retryAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    afterCancelId, 0, 0 }, retryProducer).IsOk()
                && retryProducer.Pull(secondOrdered.size()).Value()
                    == secondOrdered,
            "HTTP/3 Cancel and Reset should clear rejected-prefix bytes and ordering state");

        ActionRecorder validationActions;
        HttpBodyBudget validationBudget({ 16, 16 });
        HttpBodyBudget otherBudget({ 16, 16 });
        Http3QuicAdapter validationAdapter(&validationActions);
        HttpBodyProducer otherBudgetProducer({ 8, 1, 8 });
        HttpBodyProducer otherStreamProducer({ 8, 1, 8 });
        Require(validationAdapter.AttachBodyBudget(&validationBudget).IsOk()
                && validationAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && validationAdapter.SendStreamData(
                    12, rejectedBytes).IsOk()
                && otherBudgetProducer.AttachBudget(&otherBudget, 12).IsOk()
                && otherStreamProducer.AttachBudget(
                    &validationBudget, 16).IsOk(),
            "HTTP/3 rejected-DATA validation fixtures should be ready");
        const auto validationActionId = validationActions.actions.back().actionId;
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                validationActionId, 0, 0 }, otherBudgetProducer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should reject a different body budget");
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                validationActionId, 0, 0 }, otherStreamProducer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should reject a different budget stream");
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionAccepted,
                validationActionId, rejectedBytes.size(), 0 }, otherBudgetProducer),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 rejected-DATA requeue should reject accepted feedback");
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                validationActionId, 1, 0 }, otherBudgetProducer),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 rejected-DATA requeue should reject partially accepted feedback");
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                validationActionId + 100, 0, 0 }, otherBudgetProducer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should reject unknown feedback");
        Require(validationAdapter.ResetStream(20, 0x10C).IsOk(),
            "HTTP/3 rejected-DATA validation fixture should create control feedback");
        const auto controlActionId = validationActions.actions.back().actionId;
        RequireStatus(validationAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                controlActionId, 0, 0 }, otherBudgetProducer),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 rejected-DATA requeue should reject control feedback");
        Require(validationAdapter.Snapshot().pendingDataActions == 1
                && validationAdapter.Snapshot().pendingControlActions == 1
                && validationBudget.ReservedBytes(12) == rejectedBytes.size()
                && validationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    controlActionId, 0, 0 }).IsOk()
                && validationAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    validationActionId, rejectedBytes.size(), 0 }).IsOk(),
            "HTTP/3 rejected-DATA invalid feedback should leave pending actions intact");

        ActionRecorder unbudgetedActions;
        HttpBodyBudget adapterBudget({ 8, 8 });
        Http3QuicAdapter unbudgetedAdapter(&unbudgetedActions);
        HttpBodyProducer unbudgetedProducer({ 8, 1, 8 });
        const std::vector<std::uint8_t> oldBytes{
            'o', 'l', 'd', 'x', 'x', 'l', 'd' };
        Require(unbudgetedAdapter.AttachBodyBudget(&adapterBudget).IsOk()
                && unbudgetedAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && unbudgetedAdapter.SendStreamData(
                    24, rejectedBytes).IsOk()
                && unbudgetedProducer.Push(oldBytes.data(), oldBytes.size()).Value()
                    == oldBytes.size()
                && unbudgetedProducer.Pull(1).Value()
                    == std::vector<std::uint8_t>{ 'o' },
            "HTTP/3 unbudgeted rejected-DATA fixture should retain a partial front chunk");
        const auto unbudgetedActionId = unbudgetedActions.actions.back().actionId;
        RequireStatus(unbudgetedAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                unbudgetedActionId, 0, 0 }, unbudgetedProducer),
            LikesProgram::StatusCode::ResourceExhausted,
            "HTTP/3 rejected-DATA requeue should preserve a producer at capacity");
        Require(unbudgetedProducer.BufferedBytes() == 6
                && unbudgetedAdapter.Snapshot().pendingDataActions == 1
                && adapterBudget.ReservedBytes(24) == rejectedBytes.size()
                && unbudgetedProducer.Pull(4).Value()
                    == std::vector<std::uint8_t>({ 'l', 'd', 'x', 'x' })
                && unbudgetedAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                    Http3QuicTransportFeedbackKind::ActionRejected,
                    unbudgetedActionId, 0, 0 }, unbudgetedProducer).IsOk(),
            "HTTP/3 rejected-DATA capacity failure should retain payload for retry");
        const std::vector<std::uint8_t> expectedPartial{
            'p', 'a', 'y', 'l', 'd' };
        Require(unbudgetedProducer.Pull(expectedPartial.size()).Value()
                    == expectedPartial
                && adapterBudget.ReservedBytes() == 0
                && unbudgetedAdapter.Snapshot().pendingDataActions == 0,
            "HTTP/3 rejected-DATA requeue should normalize a partial front and release adapter budget");

        ActionRecorder cancelledActions;
        Http3QuicAdapter cancelledAdapter(&cancelledActions);
        HttpBodyProducer cancelledProducer;
        cancelledProducer.Cancel();
        Require(cancelledAdapter.Feed({
                    Http3QuicEventKind::HandshakeComplete }).IsOk()
                && cancelledAdapter.SendStreamData(28, { 'z' }).IsOk(),
            "HTTP/3 rejected-DATA cancelled producer fixture should be ready");
        const auto cancelledActionId = cancelledActions.actions.back().actionId;
        RequireStatus(cancelledAdapter.FeedTransportFeedbackAndRequeueRejectedData({
                Http3QuicTransportFeedbackKind::ActionRejected,
                cancelledActionId, 0, 0 }, cancelledProducer),
            LikesProgram::StatusCode::Cancelled,
            "HTTP/3 rejected-DATA requeue should reject a cancelled producer");
        Require(cancelledAdapter.Snapshot().pendingDataActions == 1
                && cancelledProducer.BufferedBytes() == 0
                && cancelledAdapter.FeedTransportFeedback({
                    Http3QuicTransportFeedbackKind::ActionAccepted,
                    cancelledActionId, 1, 0 }).IsOk(),
            "HTTP/3 rejected-DATA state failure should preserve the pending action");
    }

    void TestHttp3QuicRequestStreamBridge() {
        using namespace LikesProgram::Http;
        Http3QuicRequestStreamBridge bridge(7);
        const Http3Frame headers{ static_cast<std::uint64_t>(Http3FrameType::Headers),
            { 0x01 } };
        const Http3Frame data{ static_cast<std::uint64_t>(Http3FrameType::Data),
            { 'o', 'k' } };
        const auto headerBytes = BuildHttp3Frame(headers);
        const auto dataBytes = BuildHttp3Frame(data);
        Require(headerBytes.IsOk() && dataBytes.IsOk(),
            "HTTP/3 bridge fixtures should build");
        Require(bridge.Feed({ Http3QuicEventKind::StreamData, 7, 0,
                    headerBytes.Value() }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamFin, 7, 0,
                    dataBytes.Value() }).IsOk()
                && bridge.Snapshot().stream.state == Http3RequestStreamState::Complete
                && bridge.Snapshot().stream.bodyBytes == 2,
            "HTTP/3 QUIC bridge should feed stream bytes and FIN");
        RequireStatus(bridge.Feed({ Http3QuicEventKind::StreamData, 9 }),
            LikesProgram::StatusCode::InvalidArgument,
            "HTTP/3 QUIC bridge should reject a different stream id");
        bridge.Reset();
        Require(bridge.Feed({ Http3QuicEventKind::StreamReset, 7, 0x10C }).IsOk()
                && bridge.Snapshot().peerReset
                && bridge.Snapshot().transportErrorCode == 0x10C
                && bridge.FailureActions().IsOk(),
            "HTTP/3 QUIC bridge should retain peer reset without echo actions");
        RequireStatus(bridge.Feed({ Http3QuicEventKind::StreamFin, 7 }),
            LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 QUIC bridge should reject data after peer reset");
        bridge.Reset();
        Require(!bridge.Snapshot().peerReset && bridge.Snapshot().transportErrorCode == 0,
            "HTTP/3 QUIC bridge reset should clear transport terminal state");
    }

    void TestHttp3QuicControlStreamBridge() {
        using namespace LikesProgram::Http;
        Http3QuicControlStreamBridge bridge;
        const auto streamType = BuildHttp3ControlStreamType();
        const auto settings = BuildHttp3Settings({ Http3Setting{ 0x1, 0 } });
        Require(streamType.IsOk() && settings.IsOk(),
            "HTTP/3 control bridge fixtures should build");
        const Http3Frame settingsFrame{
            static_cast<std::uint64_t>(Http3FrameType::Settings), settings.Value() };
        const auto settingsBytes = BuildHttp3Frame(settingsFrame);
        Require(settingsBytes.IsOk(), "HTTP/3 control bridge SETTINGS should build");
        Require(bridge.Feed({ Http3QuicEventKind::StreamData, 0, 0,
                    streamType.Value() }).IsOk()
                && bridge.Feed({ Http3QuicEventKind::StreamData, 0, 0,
                    settingsBytes.Value() }).IsOk()
                && bridge.Snapshot().control.streamTypeAccepted
                && bridge.Snapshot().control.settingsReceived,
            "HTTP/3 control bridge should feed stream type and SETTINGS");
        bridge.Reset();
        Require(bridge.Feed({ Http3QuicEventKind::StreamReset, 0, 0x10A }).IsOk()
                && bridge.Snapshot().peerReset
                && bridge.FailureActions().IsOk(),
            "HTTP/3 control bridge should retain peer reset without close emission");
        RequireStatus(bridge.Feed({ Http3QuicEventKind::StreamData, 0, 0,
                    streamType.Value() }), LikesProgram::StatusCode::FailedPrecondition,
            "HTTP/3 control bridge should reject data after reset");
    }

    std::uint64_t NextRandom(std::uint64_t& state) noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    void TestCodecMalformedInputStability() {
        std::uint64_t state = 0x4C696B6573487474ULL; // 固定种子保证 fuzz 可复现

        for (int iteration = 0; iteration < 10000; ++iteration) {
            const std::size_t size = static_cast<std::size_t>(NextRandom(state) % 96);
            std::vector<std::uint8_t> bytes(size);            // 当前随机二进制样本
            for (auto& byte : bytes) byte = static_cast<std::uint8_t>(NextRandom(state));

            try {
                (void)LikesProgram::Http::ParseHttp2Frame(bytes);
                (void)LikesProgram::Http::ParseHttp3VarInt(bytes.data(), bytes.size());
                (void)LikesProgram::Http::ParseHttp3Frame(bytes);

                const std::string text(bytes.begin(), bytes.end()); // 随机字节按原值复制为报文样本
                (void)LikesProgram::Http::ParseHttp1Request(text);
                (void)LikesProgram::Http::ParseHttp1Response(text);
            }
            catch (...) {
                Require(false, "HTTP codec should not throw on malformed input");
            }
        }
    }

    void TestCodecConcurrentRoundTrips() {
        constexpr int threadCount = 8;                        // 并发 codec 调用线程数
        constexpr int iterationsPerThread = 2000;             // 每线程固定回归轮次
        std::atomic<int> failures{ 0 };                        // 任一线程失败即累计
        std::vector<std::thread> workers;                      // 并发回归线程集合
        workers.reserve(threadCount);

        for (int workerIndex = 0; workerIndex < threadCount; ++workerIndex) {
            workers.emplace_back([workerIndex, &failures] {
                LikesProgram::Http::HttpRequest request;
                request.method = "POST";
                request.target = "/concurrent/" + std::to_string(workerIndex);
                request.body = { 'o', 'k' };

                LikesProgram::Http::Http2Frame http2;
                http2.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
                http2.streamId = static_cast<std::uint32_t>(workerIndex * 2 + 1);
                http2.payload = request.body;

                LikesProgram::Http::Http3Frame http3;
                http3.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
                http3.payload = request.body;

                for (int iteration = 0; iteration < iterationsPerThread; ++iteration) {
                    const auto http1Bytes = LikesProgram::Http::BuildHttp1Request(request);
                    const auto http2Bytes = LikesProgram::Http::BuildHttp2Frame(http2);
                    const auto http3Bytes = LikesProgram::Http::BuildHttp3Frame(http3);
                    if (!http1Bytes.IsOk() || !http2Bytes.IsOk() || !http3Bytes.IsOk()
                        || !LikesProgram::Http::ParseHttp1Request(http1Bytes.Value()).IsOk()
                        || !LikesProgram::Http::ParseHttp2Frame(http2Bytes.Value()).IsOk()
                        || !LikesProgram::Http::ParseHttp3Frame(http3Bytes.Value()).IsOk()) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();
        Require(failures.load(std::memory_order_relaxed) == 0,
            "HTTP codecs should support independent concurrent round trips");
    }
}

int main() {
    try {
        TestPackageIdentity();
        TestHttpBodySink();
        TestHttpBodyProducer();
        TestHttpBodyBudget();
        TestHttpBodyCancellation();
        TestHttp1RequestRoundTrip();
        TestHttp1ResponseRoundTrip();
        TestHttp1Errors();
        TestHttp1TransferEncoding();
        TestHttp1LargePayload();
        TestHttp2PrefaceAndFrameRoundTrip();
        TestHttp2ErrorsAndLargePayload();
        TestHttp2SessionStateAndFlowControl();
        TestHttp2StreamBodyDecoder();
        TestHttp2HpackCodec();
        TestHttp3VarIntAndFrameRoundTrip();
        TestHttp3ErrorsAndLargePayload();
        TestHttp3StreamBodyDecoder();
        TestHttp3RequestStreamContract();
        TestHttp3RequestStreamBodySinkLifecycle();
        TestHttp3QpackResourceBudget();
        TestHttp3QpackPrefixedInteger();
        TestHttp3QpackStringLiteral();
        TestHttp3QpackHuffman();
        TestHttp3QpackInstructions();
        TestHttp3QpackInstructionApplication();
        TestHttp3QpackInstructionStreams();
        TestHttp3QpackFieldSectionPrefix();
        TestHttp3QpackFieldLines();
        TestHttp3QpackStaticTable();
        TestHttp3QpackFieldResolution();
        TestHttp3QpackFieldSectionParser();
        TestHttp3QpackFieldSectionDecoding();
        TestHttp3QpackHeaderBlockValidation();
        TestHttp3QpackSectionTracker();
        TestHttp3QpackSectionBudgetBinding();
        TestHttp3QpackDynamicTable();
        TestHttp3QpackDynamicTableBudgetBinding();
        TestHttp3ControlStream();
        TestHttpHeaderBlockValidation();
        TestHttp1ConnectionLifecycle();
        TestHttpSessionCompositionAndFailures();
        TestHttpSessionRepeatedLifecycle();
        TestHttpAltSvcCache();
        TestHttpSessionProtocolFallbackReplay();
        TestHttpConnectionPoolLifecycle();
        TestHttpWebsiteSemantics();
        TestModernWebsiteSessionRoleBoundary();
        TestHttp3QuicAdapterContract();
        TestHttp3QuicReceiveCreditTransactions();
        TestHttp3RequestSinkStreamReceiveCreditRefresh();
        TestHttp3ConnectionReceiveCreditAggregation();
        TestHttp3QuicBodyBudgetBinding();
        TestHttp3QuicPreparedProducerTransfer();
        TestHttp3QuicRejectedDataProducerRequeue();
        TestHttp3QuicRequestStreamBridge();
        TestHttp3QuicControlStreamBridge();
        TestCodecMalformedInputStability();
        TestCodecConcurrentRoundTrips();
    }
    catch (const std::exception& ex) {
        std::cerr << "HttpPackageTests failed: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "HttpPackageTests passed\n";
    return 0;
}
