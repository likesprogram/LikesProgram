#include <LikesProgram/Http/Http.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
    class ConsumerQuicActionSink final : public LikesProgram::Http::Http3QuicActionSink {
    public:
        void Submit(const LikesProgram::Http::Http3QuicAction& action) noexcept override {
            lastAction = action.kind;
            lastStreamId = action.streamId;
            lastActionId = action.actionId;
            lastErrorCode = action.errorCode;
            lastFlowCredit = action.flowCredit;
            actionIds.push_back(action.actionId);
        }

        LikesProgram::Http::Http3QuicActionKind lastAction =
            LikesProgram::Http::Http3QuicActionKind::StreamData;
        std::uint64_t lastStreamId = 0;
        std::uint64_t lastActionId = 0;
        std::uint64_t lastErrorCode = 0;
        std::uint64_t lastFlowCredit = 0;
        std::vector<std::uint64_t> actionIds;
    };

    class ConsumerQuicEventObserver final
        : public LikesProgram::Http::Http3QuicEventObserver {
    public:
        void Observe(const LikesProgram::Http::Http3QuicEvent& event) noexcept override {
            lastEvent = event.kind;
        }

        LikesProgram::Http::Http3QuicEventKind lastEvent =
            LikesProgram::Http::Http3QuicEventKind::HandshakeComplete;
    };

    class ConsumerBodyDrainObserver final
        : public LikesProgram::Http::HttpBodyDrainObserver {
    public:
        void Observe(
            const LikesProgram::Http::HttpBodyDrainEvent& event) noexcept override {
            ++events;
            lastEvent = event;
            if (bridge == nullptr || adapter == nullptr
                || event.streamId != bridge->Snapshot().stream.streamId) {
                ok = false;
                return;
            }
            if (refreshStream) {
                const auto refreshed = bridge->RefreshStreamReceiveCredit(*adapter);
                streamRefreshSubmitted = refreshed.IsOk() && refreshed.Value();
                ok = ok && streamRefreshSubmitted;
            }
            if (connectionCoordinator != nullptr) {
                const auto observed = connectionCoordinator->ObserveRequestStream(
                    bridge->Snapshot());
                if (!observed.IsOk() || !observed.Value()) {
                    ok = false;
                    return;
                }
                const auto refreshed = connectionCoordinator->
                    RefreshConnectionReceiveCredit(*adapter);
                connectionRefreshSubmitted = refreshed.IsOk() && refreshed.Value();
                ok = ok && connectionRefreshSubmitted;
            }
        }

        LikesProgram::Http::Http3QuicRequestStreamBridge* bridge = nullptr;
        LikesProgram::Http::Http3ConnectionReceiveCreditCoordinator*
            connectionCoordinator = nullptr;
        LikesProgram::Http::Http3QuicAdapter* adapter = nullptr;
        LikesProgram::Http::HttpBodyDrainEvent lastEvent{};
        std::size_t events = 0;
        bool refreshStream = false;
        bool streamRefreshSubmitted = false;
        bool connectionRefreshSubmitted = false;
        bool ok = true;
    };

    class ConsumerTransport final : public LikesProgram::Http::HttpTransport {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            LikesProgram::Http::HttpVersion version) override {
            if (version != LikesProgram::Http::HttpVersion::Http1
                && version != LikesProgram::Http::HttpVersion::Http2) {
                return LikesProgram::Status::InvalidArgument(u"consumer expects HTTP/1 or HTTP/2");
            }
            LikesProgram::Http::HttpResponse response;
            response.statusCode = request.target == "/consumer" ? 200 : 404;
            response.reason = response.statusCode == 200 ? "OK" : "Not Found";
            return response;
        }

        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            const LikesProgram::Http::HttpNegotiatedProtocol& negotiated) override {
            if (negotiated.version != LikesProgram::Http::HttpVersion::Http2
                || !negotiated.secure || negotiated.datagram || negotiated.fallback) {
                return LikesProgram::Status::InvalidArgument(
                    u"consumer expects secure HTTP/2 context");
            }
            return Exchange(request, negotiated.version);
        }
    };

    class ConsumerHandler final : public LikesProgram::Http::HttpRequestHandler {
    public:
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Handle(
            const LikesProgram::Http::HttpRequest& request) override {
            LikesProgram::Http::HttpResponse response;
            response.body.assign(request.target.begin(), request.target.end());
            return response;
        }
    };
}

int main() {
    if (!LikesProgram::Http::PackageAvailable()) return 1;
    if (std::strcmp(LikesProgram::Http::PackageName(), "LikesProgramHttp") != 0) return 2;

    LikesProgram::Http::HttpRequest request;
    request.method = "POST";
    request.target = "/consumer";
    request.headers.push_back({ "Host", "consumer.test" });
    request.body = { 'o', 'k' };

    const auto requestBytes = LikesProgram::Http::BuildHttp1Request(request);
    if (!requestBytes.IsOk()) return 3;
    const auto parsedRequest = LikesProgram::Http::ParseHttp1Request(requestBytes.Value());
    if (!parsedRequest.IsOk()) return 4;
    if (parsedRequest.Value().target != "/consumer") return 5;
    if (LikesProgram::Http::HttpHeaderValue(parsedRequest.Value().headers, "host")
        != "consumer.test") return 6;

    LikesProgram::Http::HttpRequest chunkedRequest;
    chunkedRequest.method = "POST";
    chunkedRequest.target = "/chunked";
    chunkedRequest.headers.push_back({ "Transfer-Encoding", "chunked" });
    chunkedRequest.body = { 'o', 'k' };
    chunkedRequest.trailers.push_back({ "X-Consumer-Trailer", "ok" });
    const auto chunkedBytes = LikesProgram::Http::BuildHttp1Request(chunkedRequest);
    if (!chunkedBytes.IsOk()) return 7;
    const auto parsedChunked = LikesProgram::Http::ParseHttp1Request(chunkedBytes.Value());
    if (!parsedChunked.IsOk() || parsedChunked.Value().body != chunkedRequest.body
        || parsedChunked.Value().trailers.size() != 1) return 8;

    LikesProgram::Http::Http1ChunkedDecoder decoder;
    for (char byte : "2\r\nok\r\n0\r\n\r\n") {
        if (byte == '\0') break;
        if (!decoder.Feed(std::string_view(&byte, 1)).IsOk()) return 9;
    }
    if (!decoder.Finish().IsOk() || decoder.Body() != chunkedRequest.body) return 10;

    LikesProgram::Http::HttpBodySink http1SinkBuffer({ 4, 1, 3 });
    LikesProgram::Http::Http1ChunkedDecoder http1SinkDecoder;
    if (!http1SinkDecoder.AttachBodySink(&http1SinkBuffer).IsOk()
        || !http1SinkDecoder.Feed("2\r\nok\r\n0\r\n\r\n").IsOk()
        || !http1SinkDecoder.Finish().IsOk()
        || !http1SinkBuffer.IsClosed()) return 34;
    const auto http1SinkBytes = http1SinkBuffer.Pull(8);
    if (!http1SinkBytes.IsOk() || http1SinkBytes.Value() != chunkedRequest.body) return 35;

    LikesProgram::Http::Http1Connection pipeline(
        LikesProgram::Http::Http1MessageKind::Request);
    const std::string pipelineBytes = requestBytes.Value()
        + "GET /next HTTP/1.1\r\n\r\n";
    if (!pipeline.Feed(pipelineBytes).IsOk()) return 20;
    const auto firstPipelineRequest = pipeline.NextRequest();
    const auto secondPipelineRequest = pipeline.NextRequest();
    if (!firstPipelineRequest.IsOk() || !firstPipelineRequest.Value().has_value()
        || firstPipelineRequest.Value()->target != "/consumer"
        || !secondPipelineRequest.IsOk() || !secondPipelineRequest.Value().has_value()
        || secondPipelineRequest.Value()->target != "/next"
        || pipeline.BufferedBytes() != 0) return 21;
    const auto absoluteTarget = LikesProgram::Http::ClassifyHttp1RequestTarget(
        "GET", "http://proxy.test/consumer");
    if (!absoluteTarget.IsOk()
        || absoluteTarget.Value() != LikesProgram::Http::Http1RequestTargetForm::Absolute) {
        return 22;
    }
    LikesProgram::Http::HttpResponse lifecycleResponse;
    lifecycleResponse.headers.push_back({ "Content-Length", "0" });
    const auto connectionDecision = LikesProgram::Http::EvaluateHttp1Connection(
        parsedRequest.Value(), lifecycleResponse);
    if (!connectionDecision.IsOk() || !connectionDecision.Value().keepAlive) return 23;

    const std::vector<LikesProgram::Http::HttpHeader> decodedRequestHeaders{
        { ":method", "GET" },
        { ":scheme", "https" },
        { ":authority", "consumer.test" },
        { ":path", "/consumer" }
    };
    if (!LikesProgram::Http::ValidateHttp2HeaderBlock(decodedRequestHeaders).IsOk()
        || !LikesProgram::Http::ValidateHttp3HeaderBlock(decodedRequestHeaders).IsOk()) {
        return 24;
    }
    auto forbiddenHeaders = decodedRequestHeaders;
    forbiddenHeaders.push_back({ "connection", "keep-alive" });
    if (LikesProgram::Http::ValidateHttp2HeaderBlock(forbiddenHeaders).IsOk()) return 25;

    LikesProgram::Http::Http2Session http2Session(
        true, LikesProgram::Http::Http2SessionLimits{ 16, 4, 16384 });
    if (!http2Session.OpenLocalStream(1).IsOk()
        || !http2Session.ReceiveHeaders(1, false, true).IsOk()
        || !http2Session.SendData(1, 8).IsOk()
        || !http2Session.EndStream(1, true).IsOk()
        || !http2Session.ReceiveData(1, 0, true).IsOk()
        || !http2Session.Stream(1).IsOk()
        || http2Session.Stream(1).Value().state
            != LikesProgram::Http::Http2StreamState::Closed) {
        return 26;
    }

    LikesProgram::Http::HttpBodySink bodySink({ 4, 1, 3 });
    const std::vector<std::uint8_t> sinkBytes{ 'o', 'k', '!' };
    const auto sinkAccepted = bodySink.Push(sinkBytes.data(), sinkBytes.size());
    if (!sinkAccepted.IsOk() || sinkAccepted.Value() != sinkBytes.size()
        || !bodySink.NeedsPause()) return 27;
    bodySink.Pause();
    if (!bodySink.Pull(2).IsOk() || !bodySink.CanResume()) return 28;
    bodySink.Resume();
    if (!bodySink.Close().IsOk() || !bodySink.IsClosed()) return 29;

    LikesProgram::Http::HttpBodyProducer bodyProducer({ 4, 1, 3 });
    if (!bodyProducer.Push(sinkBytes.data(), sinkBytes.size()).IsOk()) return 88;
    auto preparedBody = bodyProducer.PreparePull(2);
    if (!preparedBody.IsOk()
        || !preparedBody.Value().available
        || preparedBody.Value().payload
            != std::vector<std::uint8_t>({ 'o', 'k' })
        || bodyProducer.BufferedBytes() != sinkBytes.size()
        || bodyProducer.PreparedPullBytes() != 2
        || !bodyProducer.RollbackPull(preparedBody.Value().id).IsOk()
        || bodyProducer.BufferedBytes() != sinkBytes.size()) return 88;
    preparedBody = bodyProducer.PreparePull(2);
    if (!preparedBody.IsOk()
        || !bodyProducer.CommitPull(preparedBody.Value().id).IsOk()
        || bodyProducer.HasPreparedPull()
        || bodyProducer.BufferedBytes() != 1) return 88;

    LikesProgram::Http::Http2Frame frame;
    frame.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Settings);
    frame.streamId = 0;
    const auto frameBytes = LikesProgram::Http::BuildHttp2Frame(frame);
    if (!frameBytes.IsOk()) return 11;
    const auto parsedFrame = LikesProgram::Http::ParseHttp2Frame(frameBytes.Value());
    if (!parsedFrame.IsOk()) return 12;
    if (parsedFrame.Value().type != frame.type) return 13;
    if (std::string(LikesProgram::Http::Http2FrameTypeName(frame.type)) != "SETTINGS") return 14;

    LikesProgram::Http::Http2StreamBodyDecoder http2Body(1);
    LikesProgram::Http::Http2Frame http2Data;
    http2Data.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
    http2Data.streamId = 1;
    http2Data.flags = 0x1;
    http2Data.payload = { 'o', 'k' };
    http2Data.length = static_cast<std::uint32_t>(http2Data.payload.size());
    if (!http2Body.Feed(http2Data).IsOk() || !http2Body.Finish().IsOk()
        || http2Body.Body() != request.body) return 15;

    LikesProgram::Http::HttpBodySink http2SinkBuffer({ 4, 1, 3 });
    LikesProgram::Http::Http2StreamBodyDecoder http2SinkBody(2);
    auto http2SinkData = http2Data;
    http2SinkData.streamId = 2;
    if (!http2SinkBody.AttachBodySink(&http2SinkBuffer).IsOk()
        || !http2SinkBody.Feed(http2SinkData).IsOk()
        || !http2SinkBody.Finish().IsOk()
        || !http2SinkBuffer.IsClosed()) return 30;
    const auto http2SinkBytes = http2SinkBuffer.Pull(8);
    if (!http2SinkBytes.IsOk() || http2SinkBytes.Value() != request.body) return 31;

    LikesProgram::Http::Http3Frame http3Frame;
    http3Frame.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Settings);
    const auto http3Bytes = LikesProgram::Http::BuildHttp3Frame(http3Frame);
    if (!http3Bytes.IsOk()) return 15;
    const auto parsedHttp3 = LikesProgram::Http::ParseHttp3Frame(http3Bytes.Value());
    if (!parsedHttp3.IsOk() || parsedHttp3.Value().type != http3Frame.type) return 16;

    const auto controlType = LikesProgram::Http::BuildHttp3ControlStreamType();
    const auto controlSettings = LikesProgram::Http::BuildHttp3Settings({ { 1, 1024 } });
    const auto controlInitial =
        LikesProgram::Http::BuildHttp3ControlStreamInitialBytes({ { 1, 1024 } });
    if (!controlType.IsOk() || !controlSettings.IsOk() || !controlInitial.IsOk()
        || controlInitial.Value().size() <= controlType.Value().size()) return 72;
    const LikesProgram::Http::Http3Frame controlSettingsFrame{
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Settings),
        controlSettings.Value() };
    const auto controlFrameBytes = LikesProgram::Http::BuildHttp3Frame(
        controlSettingsFrame);
    if (!controlFrameBytes.IsOk()) return 72;
    LikesProgram::Http::Http3ControlStreamWireDecoder controlWire;
    std::vector<std::uint8_t> controlBytes = controlType.Value();
    controlBytes.insert(controlBytes.end(), controlFrameBytes.Value().begin(),
        controlFrameBytes.Value().end());
    for (const auto byte : controlBytes) {
        if (!controlWire.Feed(&byte, 1).IsOk()) return 73;
    }
    if (!controlWire.Snapshot().settingsReceived
        || controlWire.Settings().size() != 1
        || controlWire.Finish().IsOk()
        || controlWire.LastError().IsOk()) return 74;
    const auto controlActions = controlWire.FailureActions();
    if (!controlActions.IsOk()
        || !controlActions.Value().closeConnection
        || controlActions.Value().quicErrorCode
            != static_cast<std::uint64_t>(
                LikesProgram::Http::Http3ErrorCode::ClosedCriticalStream)) return 75;

    LikesProgram::Http::Http3StreamBodyDecoder http3Body(3);
    LikesProgram::Http::Http3Frame http3Data;
    http3Data.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
    http3Data.payload = request.body;
    if (!http3Body.Feed(3, http3Data, true).IsOk() || !http3Body.Finish().IsOk()
        || http3Body.Body() != request.body) return 17;

    LikesProgram::Http::HttpBodySink http3SinkBuffer({ 4, 1, 3 });
    LikesProgram::Http::Http3StreamBodyDecoder http3SinkBody(4);
    if (!http3SinkBody.AttachBodySink(&http3SinkBuffer).IsOk()
        || !http3SinkBody.Feed(4, http3Data, true).IsOk()
        || !http3SinkBody.Finish().IsOk()
        || !http3SinkBuffer.IsClosed()) return 32;
    const auto http3SinkBytes = http3SinkBuffer.Pull(8);
    if (!http3SinkBytes.IsOk() || http3SinkBytes.Value() != request.body) return 33;

    const auto requestErrorActions = LikesProgram::Http::MapHttp3RequestStreamError(
        LikesProgram::Http::Http3ErrorCode::MessageError);
    if (!requestErrorActions.IsOk()
        || requestErrorActions.Value().quicErrorCode
            != static_cast<std::uint64_t>(LikesProgram::Http::Http3ErrorCode::MessageError)
        || !requestErrorActions.Value().resetStream
        || !requestErrorActions.Value().stopSending) return 37;

    const LikesProgram::Http::Http3Frame wireHeaders{
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Headers),
        { 0x01 } };
    const LikesProgram::Http::Http3Frame wireData{
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data),
        { 'o', 'k' } };
    const auto wireHeadersBytes = LikesProgram::Http::BuildHttp3Frame(wireHeaders);
    const auto wireDataBytes = LikesProgram::Http::BuildHttp3Frame(wireData);
    if (!wireHeadersBytes.IsOk() || !wireDataBytes.IsOk()) return 69;
    const auto requestStreamBytes = LikesProgram::Http::BuildHttp3RequestStreamBytes(
        wireHeaders.payload, { wireData.payload });
    if (!requestStreamBytes.IsOk()
        || requestStreamBytes.Value().size()
            != wireHeadersBytes.Value().size() + wireDataBytes.Value().size()) return 69;
    const auto responseStreamBytes = LikesProgram::Http::BuildHttp3ResponseStreamBytes(
        wireHeaders.payload, { wireData.payload }, std::vector<std::uint8_t>{ 0x02 });
    if (!responseStreamBytes.IsOk()
        || responseStreamBytes.Value().size() <= requestStreamBytes.Value().size()) return 69;
    LikesProgram::Http::Http3RequestStreamWireDecoder wireDecoder(11);
    std::vector<std::uint8_t> wireBytes = wireHeadersBytes.Value();
    wireBytes.insert(wireBytes.end(), wireDataBytes.Value().begin(), wireDataBytes.Value().end());
    for (const auto byte : wireBytes) {
        if (!wireDecoder.Feed(&byte, 1).IsOk()) return 70;
    }
    if (!wireDecoder.Finish().IsOk()
        || !wireDecoder.IsComplete()
        || wireDecoder.PendingBodyBytes() != 0
        || wireDecoder.Snapshot().streamId != 11
        || wireDecoder.Body() != std::vector<std::uint8_t>{ 'o', 'k' }) return 71;

    LikesProgram::Http::HttpBodySink requestBridgeSink({ 2, 0, 2 });
    LikesProgram::Http::Http3QuicRequestStreamBridge requestBridge(12);
    const std::vector<std::uint8_t> requestBridgeFiller{ 'f' };
    if (!requestBridge.AttachBodySink(&requestBridgeSink).IsOk()
        || !requestBridge.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamData,
            12, 0, wireHeadersBytes.Value() }).IsOk()
        || !requestBridgeSink.Push(
            requestBridgeFiller.data(), requestBridgeFiller.size()).IsOk()
        || requestBridge.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamFin,
            12, 0, wireDataBytes.Value() }).IsOk()
        || !requestBridge.Snapshot().bodyBlocked
        || requestBridge.Snapshot().pendingBodyBytes != 2
        || requestBridge.Snapshot().bodyBufferedBytes != 1
        || requestBridge.Snapshot().bodyWritableBytes != 1
        || requestBridge.Snapshot().bodyCapacityBytes != 2
        || requestBridge.Snapshot().bodyRetryReady
        || !requestBridgeSink.Pull(1).IsOk()
        || !requestBridge.Snapshot().bodyRetryReady
        || !requestBridge.RetryPendingBody().IsOk()
        || requestBridge.Snapshot().stream.state
            != LikesProgram::Http::Http3RequestStreamState::Complete
        || requestBridge.Snapshot().stream.bodyBytes != 2
        || requestBridge.Snapshot().bodyBlocked
        || requestBridge.Snapshot().pendingBodyBytes != 0
        || requestBridge.Snapshot().bodyRetryReady
        || !requestBridgeSink.IsClosed()
        || requestBridgeSink.Pull(2).Value()
            != std::vector<std::uint8_t>({ 'o', 'k' })) return 82;

    LikesProgram::Http::Http3QuicControlStreamBridge controlBridge;
    if (!controlBridge.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamData,
            0, 0, controlType.Value() }).IsOk()
        || !controlBridge.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamData,
            0, 0, controlFrameBytes.Value() }).IsOk()
        || !controlBridge.Snapshot().control.settingsReceived) return 83;

    ConsumerQuicActionSink quicActions;
    ConsumerQuicEventObserver quicEvents;
    LikesProgram::Http::HttpBodyBudget quicPendingBudget({ 4, 3 });
    LikesProgram::Http::Http3QuicAdapter quicAdapter(&quicActions, &quicEvents);
    if (!quicAdapter.AttachBodyBudget(&quicPendingBudget).IsOk()
        || !quicAdapter.HasBodyBudget()
        || quicAdapter.SetDeadline(LikesProgram::Time::Deadline::FromNow(
            LikesProgram::Time::Duration::zero())).IsOk()
        || !quicAdapter.LastErrorContext().deadlineExpired
        || !quicAdapter.SetDeadline(LikesProgram::Time::Deadline::Infinite()).IsOk()
        || quicAdapter.LastErrorContext().deadlineExpired) return 68;
    if (!quicAdapter.Feed({ LikesProgram::Http::Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !quicAdapter.Feed({ LikesProgram::Http::Http3QuicEventKind::ConnectionSendCredit,
            0, 0, {}, 4 }).IsOk()
        || !quicAdapter.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamSendCredit,
            1, 0, {}, 3 }).IsOk()) return 36;

    ConsumerQuicActionSink receiveCreditActions;
    LikesProgram::Http::Http3QuicAdapter receiveCreditAdapter(&receiveCreditActions);
    if (!receiveCreditAdapter.Feed({
            LikesProgram::Http::Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !receiveCreditAdapter.Feed({
            LikesProgram::Http::Http3QuicEventKind::StreamData,
            9, 0, { 'x' } }).IsOk()
        || !receiveCreditAdapter.UpdateConnectionReceiveCredit(64).IsOk()
        || receiveCreditActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::ConnectionReceiveCredit
        || receiveCreditActions.lastFlowCredit != 64
        || receiveCreditAdapter.Snapshot().pendingReceiveCreditActions != 1
        || !receiveCreditAdapter.ReceiveCredit(9).Value().connectionUpdatePending) {
        return 90;
    }
    const auto connectionReceiveCreditId = receiveCreditActions.lastActionId;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            connectionReceiveCreditId, 0, 0 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(9).Value().connectionCredit != 64
        || !receiveCreditAdapter.UpdateStreamReceiveCredit(9, 32).IsOk()
        || receiveCreditActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::StreamReceiveCredit
        || receiveCreditActions.lastStreamId != 9
        || receiveCreditActions.lastFlowCredit != 32) return 90;
    const auto streamReceiveCreditId = receiveCreditActions.lastActionId;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            streamReceiveCreditId, 0, 0x71 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(9).Value().streamCreditSet
        || receiveCreditAdapter.ReceiveCredit(9).Value().streamUpdatePending
        || !receiveCreditAdapter.UpdateStreamReceiveCredit(9, 32).IsOk()) return 90;
    const auto retriedStreamReceiveCreditId = receiveCreditActions.lastActionId;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            retriedStreamReceiveCreditId, 0, 0 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(9).Value().streamCredit != 32
        || receiveCreditAdapter.Snapshot().pendingReceiveCreditActions != 0) return 90;

    LikesProgram::Http::HttpBodySink receiveCreditSink({ 8, 2, 8 });
    LikesProgram::Http::Http3QuicRequestStreamBridge receiveCreditBridge(9);
    ConsumerBodyDrainObserver receiveCreditDrain;
    receiveCreditDrain.bridge = &receiveCreditBridge;
    receiveCreditDrain.adapter = &receiveCreditAdapter;
    receiveCreditDrain.refreshStream = true;
    const auto receiveCreditHeaders = LikesProgram::Http::BuildHttp3Frame({
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Headers),
        { 0x01 } });
    const auto receiveCreditData = LikesProgram::Http::BuildHttp3Frame({
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data),
        { 'r', 'x' } });
    if (!receiveCreditHeaders.IsOk() || !receiveCreditData.IsOk()
        || !receiveCreditBridge.AttachBodySink(&receiveCreditSink).IsOk()
        || !receiveCreditSink.AttachDrainObserver(
            &receiveCreditDrain, 9).IsOk()
        || !receiveCreditSink.HasDrainObserver()
        || receiveCreditSink.DrainObserverStreamId() != 9
        || !receiveCreditBridge.ConfigureStreamReceiveCredit(32).IsOk()
        || !receiveCreditBridge.Feed({
            LikesProgram::Http::Http3QuicEventKind::StreamData,
            9, 0, receiveCreditHeaders.Value() }).IsOk()
        || !receiveCreditBridge.Feed({
            LikesProgram::Http::Http3QuicEventKind::StreamData,
            9, 0, receiveCreditData.Value() }).IsOk()
        || receiveCreditSink.Pull(1).Value()
            != std::vector<std::uint8_t>({ 'r' })
        || receiveCreditSink.PulledBytes() != 1
        || !receiveCreditDrain.ok
        || receiveCreditDrain.events != 1
        || receiveCreditDrain.lastEvent.bytes != 1
        || receiveCreditDrain.lastEvent.totalPulledBytes != 1
        || receiveCreditDrain.lastEvent.remainingBufferedBytes != 1
        || !receiveCreditDrain.streamRefreshSubmitted
        || receiveCreditBridge.Snapshot().bodyPulledSinceAttach != 1
        || receiveCreditBridge.Snapshot().streamReceiveCreditTargetLimit != 33
        || receiveCreditActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::StreamReceiveCredit
        || receiveCreditActions.lastFlowCredit != 33) return 91;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            receiveCreditActions.lastActionId, 0, 0 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(9).Value().streamCredit != 33
        || receiveCreditBridge.RefreshStreamReceiveCredit(
            receiveCreditAdapter).Value()) return 91;

    LikesProgram::Http::HttpBodySink secondReceiveCreditSink({ 8, 2, 8 });
    LikesProgram::Http::Http3QuicRequestStreamBridge secondReceiveCreditBridge(13);
    ConsumerBodyDrainObserver secondReceiveCreditDrain;
    secondReceiveCreditDrain.bridge = &secondReceiveCreditBridge;
    secondReceiveCreditDrain.adapter = &receiveCreditAdapter;
    LikesProgram::Http::Http3ConnectionReceiveCreditCoordinator
        connectionReceiveCredit;
    if (!secondReceiveCreditBridge.AttachBodySink(
            &secondReceiveCreditSink).IsOk()
        || !secondReceiveCreditSink.AttachDrainObserver(
            &secondReceiveCreditDrain, 13).IsOk()
        || !secondReceiveCreditBridge.Feed({
            LikesProgram::Http::Http3QuicEventKind::StreamData,
            13, 0, receiveCreditHeaders.Value() }).IsOk()
        || !secondReceiveCreditBridge.Feed({
            LikesProgram::Http::Http3QuicEventKind::StreamData,
            13, 0, receiveCreditData.Value() }).IsOk()
        || !connectionReceiveCredit.Configure(64).IsOk()
        || !connectionReceiveCredit.RegisterRequestStream(
            receiveCreditBridge.Snapshot()).IsOk()
        || !connectionReceiveCredit.RegisterRequestStream(
            secondReceiveCreditBridge.Snapshot()).IsOk()) return 92;
    receiveCreditDrain.refreshStream = false;
    receiveCreditDrain.connectionCoordinator = &connectionReceiveCredit;
    secondReceiveCreditDrain.connectionCoordinator = &connectionReceiveCredit;
    if (receiveCreditSink.Pull(1).Value()
            != std::vector<std::uint8_t>({ 'x' })
        || !receiveCreditDrain.ok
        || !receiveCreditDrain.connectionRefreshSubmitted
        || receiveCreditActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::ConnectionReceiveCredit
        || receiveCreditActions.lastFlowCredit != 65) return 92;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            receiveCreditActions.lastActionId, 0, 0 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(0).Value().connectionCredit != 65
        || secondReceiveCreditSink.Pull(1).Value()
            != std::vector<std::uint8_t>({ 'r' })
        || !secondReceiveCreditDrain.ok
        || secondReceiveCreditDrain.events != 1
        || !secondReceiveCreditDrain.connectionRefreshSubmitted
        || receiveCreditActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::ConnectionReceiveCredit
        || receiveCreditActions.lastFlowCredit != 66) return 92;
    if (!receiveCreditAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            receiveCreditActions.lastActionId, 0, 0 }).IsOk()
        || receiveCreditAdapter.ReceiveCredit(0).Value().connectionCredit != 66
        || connectionReceiveCredit.Snapshot().registeredStreams != 2
        || connectionReceiveCredit.Snapshot().aggregatePulledBytes != 2
        || connectionReceiveCredit.Snapshot().targetLimit != 66
        || !connectionReceiveCredit.UnregisterRequestStream(9).IsOk()
        || connectionReceiveCredit.Snapshot().aggregatePulledBytes != 2) return 92;

    LikesProgram::Http::HttpBodyProducer quicProducer({ 4, 1, 4 });
    const std::vector<std::uint8_t> preparedQuicBytes{ 'p' };
    if (!quicProducer.AttachBudget(&quicPendingBudget, 1).IsOk()
        || !quicProducer.Push(
            preparedQuicBytes.data(), preparedQuicBytes.size()).IsOk()) return 89;
    auto preparedQuicBody = quicProducer.PreparePull(preparedQuicBytes.size());
    if (!preparedQuicBody.IsOk()
        || !quicAdapter.SendPreparedStreamData(
            1, quicProducer, preparedQuicBody.Value().id).IsOk()
        || quicProducer.HasPreparedPull()
        || quicProducer.BufferedBytes() != 0
        || quicPendingBudget.ReservedBytes(1) != preparedQuicBytes.size()
        || quicAdapter.Snapshot().pendingBodyBudgetBytes
            != preparedQuicBytes.size()) return 89;
    const auto preparedQuicActionId = quicActions.lastActionId;
    const std::vector<std::uint8_t> laterQuicBytes{ 'q' };
    const std::vector<std::uint8_t> newerRejectedQuicBytes{ 'r' };
    if (!quicProducer.Push(laterQuicBytes.data(), laterQuicBytes.size()).IsOk()
        || !quicAdapter.SendStreamData(1, newerRejectedQuicBytes).IsOk()) return 89;
    const auto newerRejectedQuicActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedbackAndRequeueRejectedData({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            newerRejectedQuicActionId, 0, 0x72 }, quicProducer).IsOk()
        || !quicAdapter.FeedTransportFeedbackAndRequeueRejectedData({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            preparedQuicActionId, 0, 0x71 }, quicProducer).IsOk()
        || quicAdapter.Snapshot().pendingDataActions != 0
        || quicPendingBudget.ReservedBytes(1) != 3) return 89;
    const auto replaySnapshot = quicProducer.RejectedDataReplay();
    auto preparedReplay = quicProducer.PrepareRejectedDataReplay();
    if (!quicProducer.HasRejectedDataReplay()
        || quicProducer.RejectedDataReplayBytes() != 2
        || !replaySnapshot.available
        || replaySnapshot.streamId != 1
        || replaySnapshot.lowestActionId != preparedQuicActionId
        || replaySnapshot.bytes != 2
        || !preparedReplay.IsOk()
        || preparedReplay.Value().payload
            != std::vector<std::uint8_t>({ 'p', 'r' })
        || !quicAdapter.SendPreparedStreamData(
            replaySnapshot.streamId, quicProducer,
            preparedReplay.Value().id).IsOk()
        || quicProducer.HasRejectedDataReplay()
        || quicProducer.RejectedDataReplay().available) return 89;
    const auto preparedReplayActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            preparedReplayActionId, 2, 0 }).IsOk()) return 89;
    auto requeuedQuicTail = quicProducer.Pull(1);
    if (!requeuedQuicTail.IsOk()
        || requeuedQuicTail.Value() != laterQuicBytes
        || quicPendingBudget.ReservedBytes() != 0) return 89;
    if (!quicAdapter.Feed({ LikesProgram::Http::Http3QuicEventKind::ConnectionSendCredit,
            0, 0, {}, 4 }).IsOk()
        || !quicAdapter.Feed({ LikesProgram::Http::Http3QuicEventKind::StreamSendCredit,
            1, 0, {}, 3 }).IsOk()
        || !quicAdapter.SendStreamData(1, request.body).IsOk()
        || quicActions.lastAction != LikesProgram::Http::Http3QuicActionKind::StreamData
        || quicActions.lastStreamId != 1
        || quicActions.lastActionId == 0
        || quicPendingBudget.ReservedBytes(1) != request.body.size()
        || quicAdapter.Snapshot().pendingDataBytes != request.body.size()
        || quicAdapter.Snapshot().pendingBodyBudgetBytes != request.body.size()
        || !quicAdapter.PendingData(1).IsOk()
        || quicAdapter.PendingData(1).Value().actions != 1
        || quicAdapter.PendingData(1).Value().bytes != request.body.size()
        || quicAdapter.PendingData(1).Value().bodyBudgetBytes != request.body.size()
        || quicEvents.lastEvent
            != LikesProgram::Http::Http3QuicEventKind::StreamSendCredit) return 36;
    const auto dataActionId = quicActions.lastActionId;
    auto rejectedData = quicAdapter.FeedTransportFeedbackWithRejectedData({
        LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
        dataActionId, 0, 0x71 });
    if (!rejectedData.IsOk()
        || !rejectedData.Value().available
        || rejectedData.Value().actionId != dataActionId
        || rejectedData.Value().streamId != 1
        || rejectedData.Value().payload != request.body
        || quicAdapter.Snapshot().pendingDataActions != 0
        || quicAdapter.Snapshot().pendingDataBytes != 0
        || quicAdapter.PendingData(1).Value().actions != 0
        || quicPendingBudget.ReservedBytes() != 0) {
        return 64;
    }
    if (!quicAdapter.SendStreamData(1, rejectedData.Value().payload).IsOk()) {
        return 64;
    }
    const auto replayActionId = quicActions.lastActionId;
    auto acceptedData = quicAdapter.FeedTransportFeedbackWithRejectedData({
        LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
        replayActionId, request.body.size(), 0 });
    if (!acceptedData.IsOk()
        || acceptedData.Value().available
        || !acceptedData.Value().payload.empty()
        || quicAdapter.Snapshot().pendingDataActions != 0
        || quicAdapter.Snapshot().pendingDataBytes != 0
        || quicAdapter.PendingData(1).Value().actions != 0
        || quicPendingBudget.ReservedBytes() != 0
        || !quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::Blocked }).IsOk()
        || quicAdapter.SendStreamData(1, { 'x' }).IsOk()
        || !quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::Writable }).IsOk()) {
        return 64;
    }
    const auto quicCredit = quicAdapter.SendCredit(1);
    if (!quicCredit.IsOk()
        || !quicCredit.Value().bounded
        || quicCredit.Value().blocked
        || quicCredit.Value().availableCredit != 1) return 62;
    if (!quicAdapter.ApplyRequestStreamError(2,
            LikesProgram::Http::Http3RequestStreamQuicActions{ 0x10E, true, true }).IsOk()) {
        return 38;
    }
    if (quicAdapter.Snapshot().pendingControlActions != 2
        || quicActions.actionIds.size() < 3) {
        return 65;
    }
    const auto resetActionId = quicActions.actionIds[quicActions.actionIds.size() - 2];
    const auto stopActionId = quicActions.actionIds.back();
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            resetActionId, 0, 0 }).IsOk()
        || !quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            stopActionId, 0, 0x77 }).IsOk()
        || quicAdapter.Snapshot().pendingControlActions != 0
        || !quicAdapter.StopSending(
            2, static_cast<std::uint64_t>(
                LikesProgram::Http::Http3ErrorCode::RequestCancelled)).IsOk()
        || quicActions.lastActionId == stopActionId
        || !quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            quicActions.lastActionId, 0, 0 }).IsOk()) {
        return 66;
    }
    if (quicAdapter.LastErrorContext().valid) {
        return 39;
    }
    if (!quicAdapter.ApplyBodyCancellation(3,
            LikesProgram::Http::HttpBodyCancelReason::DeadlineExceeded).IsOk()
        || quicActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::StopSending) {
        return 63;
    }
    if (!quicAdapter.SendStreamFin(4).IsOk()
        || quicAdapter.Snapshot().pendingTerminalActions != 1) {
        return 67;
    }
    const auto finActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            finActionId, 0, 0x77 }).IsOk()
        || !quicAdapter.SendStreamFin(4).IsOk()) {
        return 68;
    }
    const auto retryFinActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            retryFinActionId, 0, 0 }).IsOk()) {
        return 71;
    }
    if (!quicAdapter.Close(0x55).IsOk()
        || quicAdapter.State() != LikesProgram::Http::Http3QuicAdapterState::Closing
        || quicAdapter.Snapshot().pendingTerminalActions != 1) {
        return 69;
    }
    const auto closeActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            closeActionId, 0, 0x88 }).IsOk()
        || quicAdapter.State()
            != LikesProgram::Http::Http3QuicAdapterState::Ready) {
        return 70;
    }
    if (quicAdapter.SetDeadline(LikesProgram::Time::Deadline::FromNow(
            LikesProgram::Time::Duration::zero())).IsOk()
        || !quicAdapter.CloseExpiredDeadline(static_cast<std::uint64_t>(
            LikesProgram::Http::Http3ErrorCode::RequestCancelled)).IsOk()
        || quicActions.lastAction
            != LikesProgram::Http::Http3QuicActionKind::CloseConnection
        || quicActions.lastErrorCode != static_cast<std::uint64_t>(
            LikesProgram::Http::Http3ErrorCode::RequestCancelled)
        || quicAdapter.Snapshot().pendingTerminalActions != 1) {
        return 84;
    }
    const auto deadlineCloseActionId = quicActions.lastActionId;
    if (!quicAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            deadlineCloseActionId, 0, 0x89 }).IsOk()
        || quicAdapter.State()
            != LikesProgram::Http::Http3QuicAdapterState::Ready
        || !quicAdapter.DeadlineExpired()
        || !quicAdapter.SetDeadline(
            LikesProgram::Time::Deadline::Infinite()).IsOk()) {
        return 85;
    }
    const auto h2Cancellation = LikesProgram::Http::MapHttp2BodyCancellation(
        LikesProgram::Http::HttpBodyCancelReason::PeerReset);
    if (!h2Cancellation.IsOk()
        || h2Cancellation.Value().kind
            != LikesProgram::Http::Http2BodyCancellationActionKind::AlreadyHandled) {
        return 64;
    }
    if (quicAdapter.Limits().maxActiveStreams == 0) {
        return 40;
    }
    LikesProgram::Http::Http3QuicAdapter rollbackAdapter;
    if (!rollbackAdapter.SetLimits({ 1, 2, 2, 8 }).IsOk()
        || !rollbackAdapter.Feed({
            LikesProgram::Http::Http3QuicEventKind::HandshakeComplete }).IsOk()
        || rollbackAdapter.SendStreamData(80, { 'x' }).IsOk()
        || rollbackAdapter.Snapshot().activeStreams != 0) {
        return 86;
    }
    ConsumerQuicActionSink feedbackSlotActions;
    LikesProgram::Http::HttpBodyBudget feedbackSlotBudget({ 2, 2 });
    LikesProgram::Http::Http3QuicAdapter feedbackSlotAdapter(
        &feedbackSlotActions);
    if (!feedbackSlotAdapter.SetLimits({ 1, 4, 4, 8 }).IsOk()
        || !feedbackSlotAdapter.AttachBodyBudget(&feedbackSlotBudget).IsOk()
        || !feedbackSlotAdapter.Feed({
            LikesProgram::Http::Http3QuicEventKind::HandshakeComplete }).IsOk()
        || !feedbackSlotAdapter.Feed({
            LikesProgram::Http::Http3QuicEventKind::ConnectionSendCredit,
            0, 0, {}, 2 }).IsOk()
        || !feedbackSlotAdapter.SendStreamData(84, { 'x' }).IsOk()
        || feedbackSlotAdapter.Snapshot().activeStreams != 1
        || feedbackSlotAdapter.Snapshot().provisionalStreams != 1
        || feedbackSlotAdapter.Snapshot().pendingBodyBudgetBytes != 1
        || feedbackSlotAdapter.SendCredit(84).Value().connectionCredit != 1
        || !feedbackSlotAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionRejected,
            feedbackSlotActions.lastActionId, 0, 0x71 }).IsOk()
        || feedbackSlotAdapter.Snapshot().activeStreams != 0
        || feedbackSlotAdapter.Snapshot().provisionalStreams != 0
        || feedbackSlotBudget.ReservedBytes() != 0
        || feedbackSlotAdapter.SendCredit(84).Value().connectionCredit != 2
        || !feedbackSlotAdapter.SendStreamData(88, { 'y' }).IsOk()
        || !feedbackSlotAdapter.FeedTransportFeedback({
            LikesProgram::Http::Http3QuicTransportFeedbackKind::ActionAccepted,
            feedbackSlotActions.lastActionId, 1, 0 }).IsOk()
        || feedbackSlotAdapter.Snapshot().activeStreams != 1
        || feedbackSlotAdapter.Snapshot().provisionalStreams != 0) {
        return 93;
    }
    LikesProgram::Http::Http3QpackResourceBudget qpackBudget(
        LikesProgram::Http::Http3QpackLimits{ 16, 1, 32 });
    if (!qpackBudget.SetDynamicTableCapacity(16).IsOk()
        || !qpackBudget.ReserveDynamicTable(8).IsOk()
        || !qpackBudget.OpenBlockedStream(0).IsOk()
        || qpackBudget.Snapshot().blockedStreams != 1) {
        return 41;
    }
    auto qpackInteger = LikesProgram::Http::BuildHttp3QpackPrefixedInteger(
        1337, 5, 0x20);
    if (!qpackInteger.IsOk()) {
        return 42;
    }
    auto parsedQpackInteger = LikesProgram::Http::ParseHttp3QpackPrefixedInteger(
        qpackInteger.Value().data(), qpackInteger.Value().size(), 5);
    if (!parsedQpackInteger.IsOk()
        || parsedQpackInteger.Value().first != 1337
        || parsedQpackInteger.Value().second != qpackInteger.Value().size()) {
        return 43;
    }
    LikesProgram::Http::Http3QpackStringLiteral qpackString;
    qpackString.bytes = { 'o', 'k' };
    auto encodedQpackString = LikesProgram::Http::BuildHttp3QpackStringLiteral(
        qpackString, 8);
    if (!encodedQpackString.IsOk()) {
        return 44;
    }
    auto parsedQpackString = LikesProgram::Http::ParseHttp3QpackStringLiteral(
        encodedQpackString.Value().data(), encodedQpackString.Value().size(), 8);
    if (!parsedQpackString.IsOk()
        || parsedQpackString.Value().first.bytes != qpackString.bytes) {
        return 45;
    }
    const std::uint8_t qpackHuffmanZero[] = { 0x07 };
    auto decodedQpackHuffman = LikesProgram::Http::DecodeHttp3QpackHuffman(
        qpackHuffmanZero, sizeof(qpackHuffmanZero));
    if (!decodedQpackHuffman.IsOk()
        || decodedQpackHuffman.Value()
            != std::vector<std::uint8_t>({ '0' })) {
        return 57;
    }
    LikesProgram::Http::Http3QpackEncoderInstruction qpackInstruction;
    qpackInstruction.type = LikesProgram::Http::Http3QpackEncoderInstructionType::Duplicate;
    qpackInstruction.value = 1;
    auto encodedQpackInstruction =
        LikesProgram::Http::BuildHttp3QpackEncoderInstruction(qpackInstruction);
    if (!encodedQpackInstruction.IsOk()) {
        return 46;
    }
    auto parsedQpackInstruction =
        LikesProgram::Http::ParseHttp3QpackEncoderInstruction(
            encodedQpackInstruction.Value().data(),
            encodedQpackInstruction.Value().size());
    if (!parsedQpackInstruction.IsOk()
        || parsedQpackInstruction.Value().first.value != 1) {
        return 47;
    }
    LikesProgram::Http::Http3QpackDynamicTable qpackTable(64);
    if (!qpackTable.SetCapacity(64).IsOk()
        || !qpackTable.Insert({ 'n' }, { 'v' }).IsOk()
        || !qpackTable.GetRelative(0).IsOk()) {
        return 48;
    }
    LikesProgram::Http::Http3QpackFieldSectionPrefix qpackPrefix{ 9, 6 };
    auto encodedQpackPrefix =
        LikesProgram::Http::BuildHttp3QpackFieldSectionPrefix(qpackPrefix, 100);
    if (!encodedQpackPrefix.IsOk()) {
        return 49;
    }
    auto parsedQpackPrefix =
        LikesProgram::Http::ParseHttp3QpackFieldSectionPrefix(
            encodedQpackPrefix.Value().data(), encodedQpackPrefix.Value().size(),
            100, 10);
    if (!parsedQpackPrefix.IsOk()
        || parsedQpackPrefix.Value().first.requiredInsertCount != 9
        || parsedQpackPrefix.Value().first.base != 6) {
        return 50;
    }
    LikesProgram::Http::Http3QpackFieldLine qpackFieldLine;
    qpackFieldLine.type = LikesProgram::Http::Http3QpackFieldLineType::LiteralWithLiteralName;
    qpackFieldLine.name.bytes = { 'x' };
    qpackFieldLine.value.bytes = { 'y' };
    auto encodedQpackFieldLine =
        LikesProgram::Http::BuildHttp3QpackFieldLine(qpackFieldLine);
    if (!encodedQpackFieldLine.IsOk()) {
        return 51;
    }
    auto parsedQpackFieldLine = LikesProgram::Http::ParseHttp3QpackFieldLine(
        encodedQpackFieldLine.Value().data(), encodedQpackFieldLine.Value().size());
    if (!parsedQpackFieldLine.IsOk()
        || parsedQpackFieldLine.Value().first.name.bytes != qpackFieldLine.name.bytes
        || parsedQpackFieldLine.Value().first.value.bytes != qpackFieldLine.value.bytes) {
        return 52;
    }
    auto qpackStaticPath = LikesProgram::Http::GetHttp3QpackStaticEntry(1);
    if (LikesProgram::Http::Http3QpackStaticTableSize() != 99
        || !qpackStaticPath.IsOk()
        || qpackStaticPath.Value().name != ":path"
        || qpackStaticPath.Value().value != "/") {
        return 53;
    }
    LikesProgram::Http::Http3QpackFieldLine qpackStaticField;
    qpackStaticField.type = LikesProgram::Http::Http3QpackFieldLineType::Indexed;
    qpackStaticField.staticTable = true;
    qpackStaticField.index = 1;
    auto resolvedQpackStaticField = LikesProgram::Http::ResolveHttp3QpackFieldLine(
        qpackStaticField, LikesProgram::Http::Http3QpackFieldSectionPrefix{},
        qpackTable);
    if (!resolvedQpackStaticField.IsOk()
        || resolvedQpackStaticField.Value().value.bytes
            != std::vector<std::uint8_t>({ '/' })) {
        return 54;
    }
    auto qpackSectionPrefix =
        LikesProgram::Http::BuildHttp3QpackFieldSectionPrefix({}, 64);
    auto qpackMethodField = qpackStaticField;
    qpackMethodField.index = 17;
    auto qpackSchemeField = qpackStaticField;
    qpackSchemeField.index = 23;
    auto qpackMethodFieldBytes =
        LikesProgram::Http::BuildHttp3QpackFieldLine(qpackMethodField);
    auto qpackSchemeFieldBytes =
        LikesProgram::Http::BuildHttp3QpackFieldLine(qpackSchemeField);
    auto qpackStaticFieldBytes =
        LikesProgram::Http::BuildHttp3QpackFieldLine(qpackStaticField);
    if (!qpackSectionPrefix.IsOk() || !qpackMethodFieldBytes.IsOk()
        || !qpackSchemeFieldBytes.IsOk() || !qpackStaticFieldBytes.IsOk()) {
        return 55;
    }
    auto qpackSectionBytes = qpackSectionPrefix.MoveValue();
    qpackSectionBytes.insert(qpackSectionBytes.end(),
        qpackMethodFieldBytes.Value().begin(), qpackMethodFieldBytes.Value().end());
    qpackSectionBytes.insert(qpackSectionBytes.end(),
        qpackSchemeFieldBytes.Value().begin(), qpackSchemeFieldBytes.Value().end());
    qpackSectionBytes.insert(qpackSectionBytes.end(),
        qpackStaticFieldBytes.Value().begin(), qpackStaticFieldBytes.Value().end());
    auto parsedQpackSection = LikesProgram::Http::ParseHttp3QpackFieldSection(
        qpackSectionBytes.data(), qpackSectionBytes.size(), qpackTable);
    if (!parsedQpackSection.IsOk()
        || parsedQpackSection.Value().fields.size() != 3
        || parsedQpackSection.Value().hasHuffman) {
        return 56;
    }
    auto decodedQpackSection = LikesProgram::Http::DecodeHttp3QpackHeaderBlock(
        parsedQpackSection.Value());
    if (!decodedQpackSection.IsOk()
        || decodedQpackSection.Value().size() != 3
        || decodedQpackSection.Value()[0].value != "GET") {
        return 57;
    }
    LikesProgram::Http::Http3RequestStream qpackRequestStream(9);
    const LikesProgram::Http::Http3Frame qpackRequestHeaders{
        static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Headers),
        qpackSectionBytes };
    auto qpackRequestHeaderResult = qpackRequestStream.Feed(
        qpackRequestHeaders, true);
    auto qpackRequestDecoded = qpackRequestStream.DecodeRequestHeaders(
        qpackTable, qpackBudget);
    if (!qpackRequestHeaderResult.IsOk()
        || !qpackRequestDecoded.IsOk()
        || qpackRequestDecoded.Value().size() != 3
        || qpackRequestDecoded.Value()[0].value != "GET") {
        return 60;
    }
    LikesProgram::Http::Http3QpackSectionTracker qpackRequestTracker;
    if (!qpackRequestStream.TrackRequestHeaderSection(
            qpackRequestTracker, qpackTable).IsOk()
        || !qpackRequestTracker.AcknowledgeSection(9).IsOk()
        || qpackRequestTracker.Snapshot().acknowledgedSections != 1) {
        return 61;
    }
    LikesProgram::Http::Http3QpackSectionTracker qpackTracker({ 2, 1 });
    if (!qpackBudget.CloseBlockedStream(0).IsOk()
        || !qpackTracker.AttachResourceBudget(&qpackBudget).IsOk()
        || !qpackTracker.HasResourceBudget()
        || !qpackTracker.SetKnownInsertCount(1).IsOk()
        || !qpackTracker.OpenSection(4, 2).IsOk()
        || qpackTracker.Snapshot().blockedStreams != 1
        || qpackBudget.Snapshot().blockedStreams != 1
        || !qpackTracker.SetKnownInsertCount(2).IsOk()
        || qpackTracker.Snapshot().blockedStreams != 0
        || qpackBudget.Snapshot().blockedStreams != 0) {
        return 58;
    }
    auto qpackUnblockedStreams = qpackTracker.TakeUnblockedStreams();
    if (!qpackUnblockedStreams.IsOk()
        || qpackUnblockedStreams.Value()
            != std::vector<std::uint64_t>({ 4 })
        || !qpackTracker.AcknowledgeSection(4).IsOk()) {
        return 58;
    }
    LikesProgram::Http::Http3QpackDecoderInstruction qpackIncrement;
    qpackIncrement.type =
        LikesProgram::Http::Http3QpackDecoderInstructionType::InsertCountIncrement;
    qpackIncrement.value = 1;
    if (!LikesProgram::Http::ApplyHttp3QpackDecoderInstruction(
            qpackIncrement, qpackTracker).IsOk()
        || qpackTracker.Snapshot().knownInsertCount != 3) {
        return 59;
    }
    LikesProgram::Http::Http3QpackResourceBudget qpackStreamBudget(
        LikesProgram::Http::Http3QpackLimits{ 64, 1, 64 });
    LikesProgram::Http::Http3QpackDynamicTable qpackStreamTable(64);
    if (!qpackStreamTable.AttachResourceBudget(&qpackStreamBudget).IsOk()
        || !qpackStreamTable.HasResourceBudget()) {
        return 87;
    }
    LikesProgram::Http::Http3QpackEncoderStream qpackEncoderStream(
        qpackStreamTable, { 64, 4 });
    const std::uint8_t qpackCapacityInstruction[] = { 0x3F, 0x00 };
    if (!qpackEncoderStream.Feed(qpackCapacityInstruction,
            sizeof(qpackCapacityInstruction)).IsOk()
        || !qpackEncoderStream.Finish().IsOk()
        || qpackStreamTable.Snapshot().capacity != 31
        || qpackStreamBudget.Snapshot().dynamicTableCapacity != 31) {
        return 60;
    }
    LikesProgram::Http::Http3QpackEncoderStream qpackEncoderFailure(
        qpackStreamTable);
    const std::uint8_t incompleteQpackInstruction[] = { 0x3F };
    if (!qpackEncoderFailure.Feed(incompleteQpackInstruction,
            sizeof(incompleteQpackInstruction)).IsOk()
        || qpackEncoderFailure.Finish().IsOk()) return 76;
    const auto encoderFailureActions = qpackEncoderFailure.FailureActions();
    if (!encoderFailureActions.IsOk()
        || !encoderFailureActions.Value().closeConnection
        || encoderFailureActions.Value().quicErrorCode
            != static_cast<std::uint64_t>(
                LikesProgram::Http::Http3QpackStreamErrorCode::EncoderStreamError)) {
        return 77;
    }
    LikesProgram::Http::Http3QpackDecoderStream qpackDecoderFailure(qpackTracker);
    const std::uint8_t incompleteQpackDecoderInstruction[] = { 0x00 };
    if (!qpackDecoderFailure.Feed(incompleteQpackDecoderInstruction,
            sizeof(incompleteQpackDecoderInstruction)).IsOk()
        || qpackDecoderFailure.Finish().IsOk()) return 78;
    const auto decoderFailureActions = qpackDecoderFailure.FailureActions();
    if (!decoderFailureActions.IsOk()
        || !decoderFailureActions.Value().closeConnection
        || decoderFailureActions.Value().quicErrorCode
            != static_cast<std::uint64_t>(
                LikesProgram::Http::Http3QpackStreamErrorCode::DecoderStreamError)) {
        return 79;
    }
    const auto qpackSectionFailure =
        LikesProgram::Http::MapHttp3QpackFieldSectionFailure(
            LikesProgram::Status::InvalidArgument(u"field section failure"));
    if (!qpackSectionFailure.IsOk()
        || !qpackSectionFailure.Value().closeConnection
        || qpackSectionFailure.Value().quicErrorCode
            != static_cast<std::uint64_t>(
                LikesProgram::Http::Http3QpackStreamErrorCode::DecompressionFailed)) {
        return 80;
    }
    const auto requestStreamQpackFailure =
        LikesProgram::Http::MapHttp3RequestStreamQpackFailure(
            LikesProgram::Status::InvalidArgument(u"request stream field failure"));
    if (!requestStreamQpackFailure.IsOk()
        || !requestStreamQpackFailure.Value().closeConnection
        || requestStreamQpackFailure.Value().quicErrorCode
            != static_cast<std::uint64_t>(
                LikesProgram::Http::Http3QpackStreamErrorCode::DecompressionFailed)) {
        return 81;
    }

    ConsumerTransport transport;
    LikesProgram::Http::HttpSession client(&transport);
    if (!client.SetNegotiatedAlpn("h2", true, false)) return 20;
    const auto negotiated = client.NegotiatedProtocol();
    if (negotiated.version != LikesProgram::Http::HttpVersion::Http2
        || !negotiated.secure
        || negotiated.datagram
        || negotiated.fallback) return 21;
    const auto sessionResponse = client.Send(request);
    if (!sessionResponse.IsOk() || sessionResponse.Value().statusCode != 200) return 18;

    LikesProgram::Http::HttpAltSvcCache altSvc;
    if (!altSvc.Observe("https://consumer.test", "h3=\":443\"; ma=60", 100).IsOk()) return 22;
    const auto h3Selection = altSvc.Select("https://consumer.test", 101);
    if (!h3Selection.IsOk()
        || h3Selection.Value().version != LikesProgram::Http::HttpVersion::Http3
        || h3Selection.Value().fallback) return 23;
    if (!LikesProgram::Http::TryApplyHttpAltSvcSelection(
            client, h3Selection.Value(), true, true)) return 25;
    if (client.NegotiatedProtocol().version != LikesProgram::Http::HttpVersion::Http3
        || !client.NegotiatedProtocol().datagram) return 26;
    const auto fallbackSelection = altSvc.Select(
        "https://consumer.test", 101, LikesProgram::Http::HttpVersion::Http2, false);
    if (!fallbackSelection.IsOk()
        || fallbackSelection.Value().version != LikesProgram::Http::HttpVersion::Http2
        || !fallbackSelection.Value().fallback) return 24;

    ConsumerHandler handler;
    LikesProgram::Http::HttpSession server(nullptr, &handler);
    const auto handlerResponse = server.Handle(request);
    if (!handlerResponse.IsOk() || handlerResponse.Value().body.size() != request.target.size()) return 19;

    std::cout << LikesProgram::Http::PackageName()
        << " consumer check passed\n";
    return 0;
}
