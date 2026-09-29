#include <LikesProgram/Quic/QuicWireEngine.hpp>
#include <LikesProgram/Quic/QuicAckFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionCloseFrame.hpp>
#include <LikesProgram/Quic/QuicConnectionIdFrame.hpp>
#include <LikesProgram/Quic/QuicLongHeader.hpp>
#include <LikesProgram/Quic/QuicCryptoFrame.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Quic/QuicPingFrame.hpp>
#include <LikesProgram/Quic/QuicShortHeader.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace {
    using namespace LikesProgram::Quic;

    void Require(bool condition,
        const std::source_location& location = std::source_location::current()) {
        if (!condition) {
            std::fprintf(stderr, "require failed at %s:%u\n",
                location.file_name(), location.line());
            std::abort();
        }
    }

    LikesProgram::Net::Buffer MakeBuffer(std::span<const std::uint8_t> bytes) {
        LikesProgram::Net::Buffer buffer(0);
        buffer.Append(bytes.data(), bytes.size());
        return buffer;
    }

    LikesProgram::Net::Buffer MakeBuffer(std::string_view value) {
        LikesProgram::Net::Buffer buffer(0);
        buffer.Append(value.data(), value.size());
        return buffer;
    }

    class Sink final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            actions.emplace_back(std::move(action));
            return { QuicActionDeliveryState::Accepted,
                QuicActionDeliveryError::None, actions.back().actionId };
        }

        std::vector<QuicActionMessage> actions;
    };

    class Observer final : public QuicEventObserver {
    public:
        void Observe(QuicStreamEvent&& event) noexcept override {
            events.emplace_back(std::move(event));
        }

        std::vector<QuicStreamEvent> events;
    };

    class MarkerProvider final : public QuicPacketProtectionProvider {
    public:
        QuicPacketProtectionResult Protect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (request.associatedData.empty()
                || output.size() < request.payload.size() + 1) {
                return { QuicPacketProtectionError::OutputTooSmall, 0 };
            }
            output[0] = 0xA5;
            std::copy(request.payload.begin(), request.payload.end(), output.begin() + 1);
            return { QuicPacketProtectionError::None, request.payload.size() + 1 };
        }

        QuicPacketProtectionResult Unprotect(
            const QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (request.associatedData.empty() || request.payload.empty()
                || request.payload.front() != 0xA5
                || output.size() < request.payload.size() - 1) {
                return { QuicPacketProtectionError::AuthenticationFailed, 0 };
            }
            std::copy(request.payload.begin() + 1, request.payload.end(), output.begin());
            return { QuicPacketProtectionError::None, request.payload.size() - 1 };
        }
    };

    class HeaderMarkerProvider final : public QuicPacketHeaderProtectionProvider {
    public:
        QuicShortHeaderProtectionResult ProtectShortHeader(
            const QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

        QuicShortHeaderProtectionResult UnprotectShortHeader(
            const QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

    private:
        static QuicShortHeaderProtectionResult Transform(
            const QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation = ValidateQuicShortHeaderProtectionRequest(
                request,
                std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != QuicPacketHeaderProtectionError::None) {
                return { validation, 0, 0, 0 };
            }
            std::copy(request.packet.begin(), request.packet.end(), output.begin());
            return { QuicPacketHeaderProtectionError::None,
                request.packet.size(), 2, 1 };
        }
    };
}

void RunQuicWireEngineTests() {
    using namespace LikesProgram::Quic;

    QuicEngineOptions options;
    const std::array<std::uint8_t, 1> destinationId{ 0x07 };
    Sink sink;
    Observer observer;
    MarkerProvider provider;
    HeaderMarkerProvider headerProvider;
    options.packetHeaderProtectionProvider = &headerProvider;
    options.destinationConnectionIdLength = 1;
    options.destinationConnectionId = destinationId;
    QuicWireEngine wire(options);
    QuicEngine* engine = &wire;
    engine->SetActionSink(&sink);
    engine->SetEventObserver(&observer);
    engine->SetPacketProtectionProvider(&provider);

    const std::array<std::uint8_t, 4> tlsBytes{ 0x01, 0x02, 0x03, 0x04 };
    auto* concrete = &wire;
    Require(concrete->QueueTlsHandshakeData(MakeBuffer(tlsBytes)).Succeeded());
    const auto started = concrete->StartHandshake();
    Require(started.Succeeded());
    Require(started.HasAction(QuicAction::DatagramReady));
    Require(started.HasAction(QuicAction::ArmTimer));
    Require(started.timerDelay == std::chrono::milliseconds(25));
    Require(sink.actions.size() == 2);
    Require(sink.actions[0].kind == QuicActionKind::DatagramReady);
    Require(sink.actions[1].kind == QuicActionKind::ArmTimer);

    const auto parsed = ParseQuicLongHeader(
        sink.actions[0].payload.Peek(), sink.actions[0].payload.ReadableBytes());
    Require(parsed.IsOk());
    Require(parsed.Value().type == QuicLongPacketType::Initial);
    const auto crypto = ParseQuicCryptoFrame(
        parsed.Value().payload.data(), parsed.Value().payload.size());
    Require(crypto.IsOk());
    Require(std::vector<std::uint8_t>(crypto.Value().data.begin(), crypto.Value().data.end())
        == std::vector<std::uint8_t>(tlsBytes.begin(), tlsBytes.end()));

    const auto progress = concrete->ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::InProgress,
        QuicTlsVersion::Unknown,
        {},
        false,
        false,
        0
    });
    Require(progress.Succeeded());
    Require(concrete->State() == QuicState::Handshaking);

    const auto complete = concrete->ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::Complete,
        QuicTlsVersion::Tls13,
        "h3",
        true,
        true,
        0
    });
    Require(complete.Succeeded());
    Require(complete.HasAction(QuicAction::StreamEvent));
    Require(complete.HasAction(QuicAction::CancelTimer));
    Require(concrete->State() == QuicState::Active);
    Require(std::string_view(concrete->NegotiatedProtocol()) == "h3");
    Require(observer.events.size() == 1);

    Require(concrete->ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxData, 0, 50, 0 }).IsOk());
    Require(concrete->ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxStreamData, 12, 40, 0 }).IsOk());
    Require(concrete->Snapshot().peerConnectionCredit == 50);
    Require(concrete->Snapshot().trackedStreamCredits == 1);
    Require(!concrete->ApplyPeerFlowControl({
        QuicFlowControlFrameKind::MaxData, 0, 49, 0 }).IsOk());
    const std::vector<std::uint8_t> blockedPayload(100, 0x7a);
    const auto blockedSend = concrete->SendStreamData(
        12, MakeBuffer(std::span<const std::uint8_t>(
            blockedPayload.data(), blockedPayload.size())));
    Require(blockedSend.error
        == static_cast<int>(QuicWireEngineError::FlowControlBlocked));
    Require(concrete->State() == QuicState::Active);

    const std::array<std::uint8_t, 2> associatedData{ 0xC0, 0x01 };
    const std::array<std::uint8_t, 2> plaintext{ 0xAA, 0xBB };
    std::array<std::uint8_t, 8> protectedBytes{};
    const auto protectedResult = concrete->ProtectPacket({
        QuicPacketProtectionLevel::OneRtt, 1, associatedData, plaintext }, protectedBytes);
    Require(protectedResult.Succeeded());
    Require(protectedResult.bytesWritten == plaintext.size() + 1);

    const auto activeCrypto = BuildQuicCryptoFrame({ 16, tlsBytes });
    Require(activeCrypto.IsOk());
    std::vector<std::uint8_t> activeProtectedPayload(activeCrypto.Value().size() + 1);
    const auto activeProtectedResult = provider.Protect({
        QuicPacketProtectionLevel::Handshake,
        2,
        associatedData,
        activeCrypto.Value()
    }, activeProtectedPayload);
    Require(activeProtectedResult.Succeeded());
    activeProtectedPayload.resize(activeProtectedResult.bytesWritten);
    const auto activePeerPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Handshake,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 2 },
        activeProtectedPayload,
        {}
    });
    Require(activePeerPacket.IsOk());
    Require(concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(activePeerPacket.Value())).Succeeded());
    Require(observer.events.size() == 2);
    Require(observer.events.back().kind == QuicEventKind::CryptoData);
    Require(observer.events.back().streamId == 16);
    Require(observer.events.back().payload.ReadableBytes() == tlsBytes.size());

    const std::array<std::uint8_t, 3> streamData{ 0x61, 0x62, 0x63 };
    QuicStreamFrameBuildOptions streamOptions;
    streamOptions.streamId = 4;
    streamOptions.hasLength = true;
    streamOptions.data = streamData;
    const auto streamFrame = BuildQuicStreamFrame(streamOptions);
    Require(streamFrame.IsOk());
    std::vector<std::uint8_t> protectedStreamPayload(streamFrame.Value().size() + 1);
    protectedStreamPayload[0] = 0xA5;
    std::copy(streamFrame.Value().begin(), streamFrame.Value().end(),
        protectedStreamPayload.begin() + 1);
    const std::array<std::uint8_t, 1> shortPacketNumber{ 0x02 };
    const auto shortPacket = BuildQuicShortHeaderPacket({
        false,
        false,
        destinationId,
        shortPacketNumber,
        protectedStreamPayload
    });
    Require(shortPacket.IsOk());
    Require(concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(shortPacket.Value())).Succeeded());
    Require(observer.events.size() == 3);
    Require(observer.events.back().kind == QuicEventKind::StreamData);
    Require(observer.events.back().streamId == 4);
    Require(observer.events.back().payload.ReadableBytes() == streamData.size());
    Require(std::equal(
        observer.events.back().payload.Peek(),
        observer.events.back().payload.Peek() + streamData.size(),
        streamData.begin()));

    const auto buildStreamPacket = [&](std::uint64_t offset,
        std::span<const std::uint8_t> data,
        std::uint8_t packetNumber) {
        QuicStreamFrameBuildOptions fragmentOptions;
        fragmentOptions.streamId = 4;
        fragmentOptions.offset = offset;
        fragmentOptions.hasOffset = true;
        fragmentOptions.hasLength = true;
        fragmentOptions.data = data;
        const auto fragment = BuildQuicStreamFrame(fragmentOptions);
        Require(fragment.IsOk());
        std::vector<std::uint8_t> protectedFragment(fragment.Value().size() + 1);
        protectedFragment[0] = 0xA5;
        std::copy(fragment.Value().begin(), fragment.Value().end(),
            protectedFragment.begin() + 1);
        const auto packet = BuildQuicShortHeaderPacket({
            false,
            false,
            destinationId,
            std::array<std::uint8_t, 1>{ packetNumber },
            protectedFragment
        });
        Require(packet.IsOk());
        return packet.Value();
    };
    const std::array<std::uint8_t, 3> lateData{ 0x67, 0x68, 0x69 };
    const auto latePacket = buildStreamPacket(6, lateData, 3);
    Require(concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(latePacket)).Succeeded());
    Require(observer.events.size() == 3);
    const std::array<std::uint8_t, 3> middleData{ 0x64, 0x65, 0x66 };
    const auto middlePacket = buildStreamPacket(3, middleData, 4);
    Require(concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(middlePacket)).Succeeded());
    Require(observer.events.size() == 4);
    Require(observer.events.back().kind == QuicEventKind::StreamData);
    Require(observer.events.back().streamId == 4);
    const std::array<std::uint8_t, 6> reassembledData{
        0x64, 0x65, 0x66, 0x67, 0x68, 0x69 };
    Require(observer.events.back().payload.ReadableBytes() == reassembledData.size());
    Require(std::equal(
        observer.events.back().payload.Peek(),
        observer.events.back().payload.Peek() + reassembledData.size(),
        reassembledData.begin()));

    const auto buildActivePacket = [&](std::span<const std::uint8_t> frames,
        std::uint8_t packetNumber) {
        std::vector<std::uint8_t> protectedFrames(frames.size() + 1);
        protectedFrames[0] = 0xA5;
        std::copy(frames.begin(), frames.end(), protectedFrames.begin() + 1);
        const auto packet = BuildQuicShortHeaderPacket({
            false,
            false,
            destinationId,
            std::array<std::uint8_t, 1>{ packetNumber },
            protectedFrames
        });
        Require(packet.IsOk());
        return packet.Value();
    };
    const auto appendFrame = [](std::vector<std::uint8_t>& destination,
        const std::vector<std::uint8_t>& frame) {
        destination.insert(destination.end(), frame.begin(), frame.end());
    };

    std::vector<std::uint8_t> frameSequence{ 0x00 };
    const auto ping = BuildQuicPingFrame();
    Require(ping.IsOk());
    appendFrame(frameSequence, ping.Value());
    const std::array<std::uint8_t, 2> sequenceData{ 0x71, 0x72 };
    const auto sequenceStream = BuildQuicStreamFrame({
        12, 0, false, true, false, sequenceData });
    Require(sequenceStream.IsOk());
    appendFrame(frameSequence, sequenceStream.Value());
    const auto sequenceReset = BuildQuicResetStreamFrame({ 13, 41, 2, 0 });
    const auto sequenceStop = BuildQuicStopSendingFrame({ 14, 42, 0 });
    Require(sequenceReset.IsOk() && sequenceStop.IsOk());
    appendFrame(frameSequence, sequenceReset.Value());
    appendFrame(frameSequence, sequenceStop.Value());
    QuicAckFrame sequenceAck;
    sequenceAck.largestAcknowledged = 5;
    sequenceAck.ranges = { { 5, 5 } };
    const auto sequenceAckBytes = BuildQuicAckFrame(sequenceAck);
    const auto maxData = BuildQuicFlowControlFrame({
        QuicFlowControlFrameKind::MaxData, 0, 100, 0 });
    const auto maxStreamData = BuildQuicFlowControlFrame({
        QuicFlowControlFrameKind::MaxStreamData, 12, 50, 0 });
    Require(sequenceAckBytes.IsOk() && maxData.IsOk() && maxStreamData.IsOk());
    appendFrame(frameSequence, sequenceAckBytes.Value());
    appendFrame(frameSequence, maxData.Value());
    appendFrame(frameSequence, maxStreamData.Value());
    const std::array<std::uint8_t, 1> issuedConnectionId{ 0x31 };
    const std::array<std::uint8_t, 16> resetToken{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const auto connectionId = BuildQuicConnectionIdFrame({
        QuicConnectionIdFrameKind::NewConnectionId,
        0,
        0,
        issuedConnectionId,
        resetToken,
        0
    });
    Require(connectionId.IsOk());
    appendFrame(frameSequence, connectionId.Value());
    const std::array<std::uint8_t, 8> pathToken{
        9, 8, 7, 6, 5, 4, 3, 2 };
    const auto pathChallenge = BuildQuicPathValidationFrame({
        QuicPathValidationFrameKind::PathChallenge, pathToken, 0 });
    Require(pathChallenge.IsOk());
    appendFrame(frameSequence, pathChallenge.Value());

    const auto eventCountBeforeSequence = observer.events.size();
    const auto actionCountBeforeSequence = sink.actions.size();
    const auto sequenceResult = concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(buildActivePacket(frameSequence, 5)));
    Require(sequenceResult.Succeeded());
    Require(sequenceResult.HasAction(QuicAction::StreamEvent));
    Require(sequenceResult.HasAction(QuicAction::DatagramReady));
    Require(sequenceResult.HasAction(QuicAction::ArmTimer));
    Require(observer.events.size() == eventCountBeforeSequence + 3);
    Require(observer.events[eventCountBeforeSequence].kind
        == QuicEventKind::StreamData);
    Require(observer.events[eventCountBeforeSequence].streamId == 12);
    Require(observer.events[eventCountBeforeSequence + 1].kind
        == QuicEventKind::StreamReset);
    Require(observer.events[eventCountBeforeSequence + 1].streamId == 13);
    Require(observer.events[eventCountBeforeSequence + 1].errorCode == 41);
    Require(observer.events[eventCountBeforeSequence + 2].kind
        == QuicEventKind::StopSending);
    Require(observer.events[eventCountBeforeSequence + 2].streamId == 14);
    Require(observer.events[eventCountBeforeSequence + 2].errorCode == 42);
    Require(sink.actions.size() == actionCountBeforeSequence + 2);
    Require(sink.actions[actionCountBeforeSequence].kind == QuicActionKind::ArmTimer);
    Require(sink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(sink.actions.back().peer.Port() == 445);
    const auto pathResponseHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(pathResponseHeader.IsOk()
        && pathResponseHeader.Value().payload.size() > 1
        && pathResponseHeader.Value().payload.front() == 0xA5);
    const auto pathResponseFrame = ParseQuicPathValidationFrame(
        pathResponseHeader.Value().payload.data() + 1,
        pathResponseHeader.Value().payload.size() - 1);
    Require(pathResponseFrame.IsOk()
        && pathResponseFrame.Value().kind
            == QuicPathValidationFrameKind::PathResponse
        && pathResponseFrame.Value().data == pathToken);

    const auto sequenceSnapshot = concrete->Snapshot();
    Require(sequenceSnapshot.observedAckFrames == 1);
    Require(sequenceSnapshot.largestAcknowledgedPacket == 5);
    Require(sequenceSnapshot.peerConnectionCredit == 100);
    Require(sequenceSnapshot.trackedStreamCredits == 1);
    Require(sequenceSnapshot.congestionWindowBytes == 12000);
    Require(!sequenceSnapshot.pathChallengePending);
    Require(sequenceSnapshot.hasConnectionIdSequence);
    Require(sequenceSnapshot.highestConnectionIdSequence == 0);
    Require(sequenceSnapshot.pathState == QuicPathState::Validating);

    const std::array<std::uint8_t, 8> migrationToken{
        1, 3, 5, 7, 9, 11, 13, 15 };
    const LikesProgram::Net::Address candidatePeer("127.0.0.1", 447);
    const auto beginActionCount = sink.actions.size();
    const auto beginMigration = engine->BeginPathValidation(
        candidatePeer, migrationToken);
    Require(beginMigration.Succeeded());
    Require(beginMigration.HasAction(QuicAction::DatagramReady));
    Require(beginMigration.HasAction(QuicAction::ArmTimer));
    Require(sink.actions.size() == beginActionCount + 2);
    Require(sink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(sink.actions.back().peer.Port() == 447);
    const auto outboundChallengeHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(outboundChallengeHeader.IsOk()
        && outboundChallengeHeader.Value().payload.size() > 1
        && outboundChallengeHeader.Value().payload.front() == 0xA5);
    const auto outboundChallenge = ParseQuicPathValidationFrame(
        outboundChallengeHeader.Value().payload.data() + 1,
        outboundChallengeHeader.Value().payload.size() - 1);
    Require(outboundChallenge.IsOk()
        && outboundChallenge.Value().kind
            == QuicPathValidationFrameKind::PathChallenge
        && outboundChallenge.Value().data == migrationToken);
    Require(concrete->Snapshot().pathChallengePending);

    const auto rejectedEventCount = observer.events.size();
    const auto candidateApplication = concrete->ConsumeDatagram(
        candidatePeer,
        MakeBuffer(buildActivePacket(sequenceStream.Value(), 6)));
    Require(candidateApplication.error
        == static_cast<int>(QuicWireEngineError::InvalidDatagram));
    Require(concrete->State() == QuicState::Active);
    Require(observer.events.size() == rejectedEventCount);
    Require(concrete->Snapshot().pathChallengePending);

    const auto accountedStream = concrete->SendStreamData(12, MakeBuffer("credit"));
    Require(accountedStream.Succeeded());
    Require(accountedStream.HasAction(QuicAction::ArmTimer));
    Require(sink.actions.back().peer.Port() == 445);
    Require(concrete->Snapshot().reservedConnectionCredit > 0);
    Require(concrete->Snapshot().congestionInFlightBytes > 0);
    Require(concrete->Snapshot().pendingRetransmissions > 0);
    Require(concrete->Snapshot().recoveryTimerArmed);

    const auto inboundPathResponse = BuildQuicPathValidationFrame({
        QuicPathValidationFrameKind::PathResponse, migrationToken, 0 });
    Require(inboundPathResponse.IsOk());
    const auto wrongPeerPathResult = concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(buildActivePacket(inboundPathResponse.Value(), 7)));
    Require(wrongPeerPathResult.error
        == static_cast<int>(QuicWireEngineError::InvalidDatagram));
    Require(concrete->State() == QuicState::Active);
    Require(concrete->Snapshot().pathChallengePending);

    const auto mismatchedPathResponse = BuildQuicPathValidationFrame({
        QuicPathValidationFrameKind::PathResponse,
        std::array<std::uint8_t, 8>{ 8, 6, 4, 2, 0, 2, 4, 6 },
        0
    });
    Require(mismatchedPathResponse.IsOk());
    const auto mismatchedPathResult = concrete->ConsumeDatagram(
        candidatePeer,
        MakeBuffer(buildActivePacket(mismatchedPathResponse.Value(), 8)));
    Require(mismatchedPathResult.error
        == static_cast<int>(QuicWireEngineError::InvalidDatagram));
    Require(concrete->State() == QuicState::Active);
    Require(concrete->Snapshot().pathChallengePending);

    const auto pathResult = concrete->ConsumeDatagram(
        candidatePeer,
        MakeBuffer(buildActivePacket(inboundPathResponse.Value(), 9)));
    Require(pathResult.Succeeded());
    Require(pathResult.HasAction(QuicAction::PathEvent));
    Require(observer.events.back().kind == QuicEventKind::PathMigrated);
    Require(observer.events.back().peer.Port() == 447);
    Require(concrete->Snapshot().pathState == QuicPathState::Migrated);
    Require(!concrete->Snapshot().pathChallengePending);

    QuicAckFrame outboundAck;
    outboundAck.largestAcknowledged = 2;
    outboundAck.ranges = { { 2, 2 } };
    const auto outboundAckBytes = BuildQuicAckFrame(outboundAck);
    Require(outboundAckBytes.IsOk());
    const auto outboundAckResult = concrete->ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 447),
        MakeBuffer(buildActivePacket(outboundAckBytes.Value(), 10)));
    Require(outboundAckResult.Succeeded());
    Require(concrete->Snapshot().reservedConnectionCredit == 0);
    Require(concrete->Snapshot().congestionInFlightBytes == 0);

    const auto sentStream = concrete->SendStreamData(9, MakeBuffer("outbound"));
    Require(sentStream.Succeeded());
    Require(sink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(sink.actions.back().peer.Port() == 447);
    const auto sentHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(sentHeader.IsOk());
    Require(sentHeader.Value().destinationConnectionId.size()
        == options.destinationConnectionIdLength);
    Require(sentHeader.Value().payload.size() > 1
        && sentHeader.Value().payload.front() == 0xA5);
    const auto sentFrame = ParseQuicStreamFrame(
        sentHeader.Value().payload.data() + 1,
        sentHeader.Value().payload.size() - 1);
    Require(sentFrame.IsOk());
    Require(sentFrame.Value().consumedBytes == sentHeader.Value().payload.size() - 1);
    Require(sentFrame.Value().streamId == 9);
    const std::array<std::uint8_t, 8> expectedOutbound{
        'o', 'u', 't', 'b', 'o', 'u', 'n', 'd' };
    Require(sentFrame.Value().data.size() == expectedOutbound.size());
    Require(sentFrame.Value().data[0] == expectedOutbound[0]);
    Require(sentFrame.Value().data[1] == expectedOutbound[1]);
    Require(sentFrame.Value().data[2] == expectedOutbound[2]);
    Require(sentFrame.Value().data[3] == expectedOutbound[3]);
    Require(sentFrame.Value().data[4] == expectedOutbound[4]);
    Require(sentFrame.Value().data[5] == expectedOutbound[5]);
    Require(sentFrame.Value().data[6] == expectedOutbound[6]);
    Require(sentFrame.Value().data[7] == expectedOutbound[7]);
    Require(!sentFrame.Value().hasOffset && sentFrame.Value().offset == 0);

    Require(concrete->SendStreamData(9, MakeBuffer("continuation")).Succeeded());
    const auto continuationHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(continuationHeader.IsOk() && continuationHeader.Value().payload.size() > 1
        && continuationHeader.Value().payload.front() == 0xA5);
    const auto continuationFrame = ParseQuicStreamFrame(
        continuationHeader.Value().payload.data() + 1,
        continuationHeader.Value().payload.size() - 1);
    Require(continuationFrame.IsOk() && continuationFrame.Value().streamId == 9
        && continuationFrame.Value().hasOffset
        && continuationFrame.Value().offset == expectedOutbound.size());
    Require(continuationFrame.Value().data.size() == 12);

    Require(concrete->SendStreamFin(9).Succeeded());
    const auto finHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(finHeader.IsOk() && finHeader.Value().payload.size() > 1
        && finHeader.Value().payload.front() == 0xA5);
    const auto finFrame = ParseQuicStreamFrame(
        finHeader.Value().payload.data() + 1,
        finHeader.Value().payload.size() - 1);
    Require(finFrame.IsOk() && finFrame.Value().streamId == 9
        && finFrame.Value().hasOffset
        && finFrame.Value().offset == expectedOutbound.size() + 12
        && finFrame.Value().fin && finFrame.Value().data.empty());

    Require(concrete->ResetStream(9, 7).Succeeded());
    const auto resetHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(resetHeader.IsOk() && resetHeader.Value().payload.size() > 1
        && resetHeader.Value().payload.front() == 0xA5);
    const auto resetFrame = ParseQuicResetStreamFrame(
        resetHeader.Value().payload.data() + 1,
        resetHeader.Value().payload.size() - 1);
    Require(resetFrame.IsOk() && resetFrame.Value().streamId == 9
        && resetFrame.Value().applicationErrorCode == 7
        && resetFrame.Value().finalSize == 0);

    Require(concrete->StopSending(9, 8).Succeeded());
    const auto stopHeader = ParseQuicShortHeader(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(stopHeader.IsOk() && stopHeader.Value().payload.size() > 1
        && stopHeader.Value().payload.front() == 0xA5);
    const auto stopFrame = ParseQuicStopSendingFrame(
        stopHeader.Value().payload.data() + 1,
        stopHeader.Value().payload.size() - 1);
    Require(stopFrame.IsOk() && stopFrame.Value().streamId == 9
        && stopFrame.Value().applicationErrorCode == 8);

    const std::array<std::uint8_t, 5> activeDatagram{
        0x40, 0x01, 0x02, 0xA5, 0x00 };
    const auto queuedActive = concrete->QueueProtectedDatagram(
        MakeBuffer(activeDatagram));
    Require(queuedActive.Succeeded());
    Require(queuedActive.HasAction(QuicAction::DatagramReady));
    Require(sink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(sink.actions.back().actionId != 0);
    Require(sink.actions.back().payload.ReadableBytes() == activeDatagram.size());
    Require(std::vector<std::uint8_t>(
        sink.actions.back().payload.Peek(),
        sink.actions.back().payload.Peek() + activeDatagram.size())
        == std::vector<std::uint8_t>(activeDatagram.begin(), activeDatagram.end()));
    Require(concrete->Snapshot().sentDatagrams == 10);

    const auto close = concrete->Close(7);
    Require(close.Succeeded());
    Require(concrete->State() == QuicState::Closed);
    Require(sink.actions.back().kind == QuicActionKind::CloseConnection);

    QuicWireEngine peerCloseEngine(options);
    Sink peerCloseSink;
    Observer peerCloseObserver;
    peerCloseEngine.SetActionSink(&peerCloseSink);
    peerCloseEngine.SetEventObserver(&peerCloseObserver);
    peerCloseEngine.SetPacketProtectionProvider(&provider);
    Require(peerCloseEngine.StartHandshake().Succeeded());
    Require(peerCloseEngine.ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::Complete,
        QuicTlsVersion::Tls13,
        "h3",
        true,
        true,
        0
    }).Succeeded());
    const std::array<std::uint8_t, 3> closeReason{ 'b', 'y', 'e' };
    const auto peerCloseFrame = BuildQuicConnectionCloseFrame({
        true, 55, 0, closeReason, 0 });
    Require(peerCloseFrame.IsOk());
    const auto peerCloseResult = peerCloseEngine.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 448),
        MakeBuffer(buildActivePacket(peerCloseFrame.Value(), 7)));
    Require(peerCloseResult.Succeeded());
    Require(peerCloseResult.HasAction(QuicAction::ConnectionClose));
    Require(peerCloseEngine.State() == QuicState::Closed);
    Require(peerCloseObserver.events.back().kind
        == QuicEventKind::ConnectionClose);
    Require(peerCloseObserver.events.back().applicationError);
    Require(peerCloseObserver.events.back().errorCode == 55);
    Require(peerCloseObserver.events.back().payload.ReadableBytes()
        == closeReason.size());

    // 外部 scheduler 触发 Active timeout 后，重传必须同步 recovery/credit ledger。
    QuicWireEngine recoveryEngine(options);
    Sink recoverySink;
    recoveryEngine.SetActionSink(&recoverySink);
    recoveryEngine.SetPacketProtectionProvider(&provider);
    Require(recoveryEngine.StartHandshake().Succeeded());
    Require(recoveryEngine.ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::Complete,
        QuicTlsVersion::Tls13,
        "h3",
        true,
        true,
        0
    }).Succeeded());
    std::vector<std::uint8_t> recoveryCreditFrames;
    appendFrame(recoveryCreditFrames, maxData.Value());
    appendFrame(recoveryCreditFrames, maxStreamData.Value());
    Require(recoveryEngine.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 449),
        MakeBuffer(buildActivePacket(recoveryCreditFrames, 9))).Succeeded());
    Require(recoveryEngine.SendStreamData(12, MakeBuffer("retry")).Succeeded());
    const auto initialRecoveryHeader = ParseQuicShortHeader(
        recoverySink.actions.back().payload.Peek(),
        recoverySink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(initialRecoveryHeader.IsOk()
        && initialRecoveryHeader.Value().packetNumber.size() == 1
        && initialRecoveryHeader.Value().packetNumber.front() == 0);
    Require(recoveryEngine.Snapshot().pendingRetransmissions == 1);
    Require(recoveryEngine.Snapshot().reservedConnectionCredit > 0);
    const auto timeoutActions = recoverySink.actions.size();
    const auto recoveryTimeout = recoveryEngine.HandleTimeout();
    Require(recoveryTimeout.Succeeded());
    Require(recoveryTimeout.HasAction(QuicAction::DatagramReady));
    Require(recoveryTimeout.HasAction(QuicAction::ArmTimer));
    Require(recoverySink.actions.size() == timeoutActions + 2);
    Require(recoveryEngine.Snapshot().pendingRetransmissions == 1);
    Require(recoveryEngine.Snapshot().reservedConnectionCredit > 0);
    const auto retransmittedHeader = ParseQuicShortHeader(
        recoverySink.actions.back().payload.Peek(),
        recoverySink.actions.back().payload.ReadableBytes(),
        options.destinationConnectionIdLength);
    Require(retransmittedHeader.IsOk());
    Require(retransmittedHeader.Value().packetNumber.size() == 1
        && retransmittedHeader.Value().packetNumber.front() == 1);
    Require(retransmittedHeader.Value().payload.size() > 1
        && retransmittedHeader.Value().payload.front() == 0xA5);
    const auto retransmittedFrame = ParseQuicStreamFrame(
        retransmittedHeader.Value().payload.data() + 1,
        retransmittedHeader.Value().payload.size() - 1);
    Require(retransmittedFrame.IsOk()
        && retransmittedFrame.Value().streamId == 12);
    const std::array<std::uint8_t, 5> expectedRetry{
        'r', 'e', 't', 'r', 'y' };
    Require(retransmittedFrame.Value().data.size() == expectedRetry.size());
    Require(std::equal(
        retransmittedFrame.Value().data.begin(),
        retransmittedFrame.Value().data.end(),
        expectedRetry.begin()));
    QuicAckFrame recoveryAck;
    recoveryAck.largestAcknowledged = 1;
    recoveryAck.ranges = { { 1, 1 } };
    const auto recoveryAckBytes = BuildQuicAckFrame(recoveryAck);
    Require(recoveryAckBytes.IsOk());
    Require(recoveryEngine.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 449),
        MakeBuffer(buildActivePacket(recoveryAckBytes.Value(), 10))).Succeeded());
    Require(recoveryEngine.Snapshot().pendingRetransmissions == 0);
    Require(recoveryEngine.Snapshot().reservedConnectionCredit == 0);
    Require(recoveryEngine.Snapshot().congestionInFlightBytes == 0);

    // A candidate-path probe and its timeout retransmission keep the candidate
    // target until a matching response commits the new peer atomically.
    QuicWireEngine migrationEngine(options);
    Sink migrationSink;
    Observer migrationObserver;
    migrationEngine.SetActionSink(&migrationSink);
    migrationEngine.SetEventObserver(&migrationObserver);
    migrationEngine.SetPacketProtectionProvider(&provider);
    Require(migrationEngine.StartHandshake().Succeeded());
    Require(migrationEngine.ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::Complete,
        QuicTlsVersion::Tls13,
        "h3",
        true,
        true,
        0
    }).Succeeded());
    const LikesProgram::Net::Address originalMigrationPeer("127.0.0.1", 450);
    const LikesProgram::Net::Address timeoutCandidatePeer("127.0.0.1", 451);
    Require(migrationEngine.ConsumeDatagram(
        originalMigrationPeer,
        MakeBuffer(buildActivePacket(ping.Value(), 11))).Succeeded());
    const std::array<std::uint8_t, 8> timeoutMigrationToken{
        2, 4, 6, 8, 10, 12, 14, 16 };
    Require(migrationEngine.BeginPathValidation(
        timeoutCandidatePeer, timeoutMigrationToken).Succeeded());
    Require(migrationSink.actions.back().peer.Port() == 451);
    const auto migrationTimeoutActionCount = migrationSink.actions.size();
    const auto migrationTimeout = migrationEngine.HandleTimeout();
    Require(migrationTimeout.Succeeded());
    Require(migrationTimeout.HasAction(QuicAction::DatagramReady));
    Require(migrationSink.actions.size() == migrationTimeoutActionCount + 2);
    Require(migrationSink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(migrationSink.actions.back().peer.Port() == 451);
    const auto timeoutMigrationResponse = BuildQuicPathValidationFrame({
        QuicPathValidationFrameKind::PathResponse, timeoutMigrationToken, 0 });
    Require(timeoutMigrationResponse.IsOk());
    Require(migrationEngine.ConsumeDatagram(
        timeoutCandidatePeer,
        MakeBuffer(buildActivePacket(timeoutMigrationResponse.Value(), 12))).Succeeded());
    Require(migrationEngine.Snapshot().pathState == QuicPathState::Migrated);
    Require(migrationObserver.events.back().kind == QuicEventKind::PathMigrated);
    Require(migrationObserver.events.back().peer.Port() == 451);
    Require(migrationEngine.SendStreamData(17, MakeBuffer("new-path")).Succeeded());
    Require(migrationSink.actions.back().peer.Port() == 451);

    auto timeoutEngine = CreateQuicWireEngine(options);
    Require(timeoutEngine != nullptr);
    timeoutEngine->SetActionSink(&sink);
    Require(timeoutEngine->StartHandshake().Succeeded());
    const auto timeout = timeoutEngine->HandleTimeout();
    Require(timeout.error == static_cast<int>(QuicWireEngineError::HandshakeTimeout));
    Require(timeoutEngine->State() == QuicState::Failed);

    auto invalidEngine = CreateQuicWireEngine(options);
    Require(invalidEngine != nullptr);
    invalidEngine->SetActionSink(&sink);
    Require(invalidEngine->StartHandshake().Succeeded());
    const auto invalid = invalidEngine->ProvideTlsHandshakeResult({
        QuicTlsHandshakeState::Complete,
        QuicTlsVersion::Tls13,
        "h2",
        true,
        true,
        0
    });
    Require(invalid.error != 0);
    Require(invalidEngine->State() == QuicState::Failed);

    QuicWireEngine protectedEngine(options);
    Sink protectedSink;
    protectedEngine.SetActionSink(&protectedSink);
    const std::array<std::uint8_t, 6> protectedDatagram{
        0xC0, 0x00, 0x00, 0x00, 0x01, 0xA5 };
    Require(protectedEngine.QueueProtectedDatagram(
        MakeBuffer(protectedDatagram)).Succeeded());
    Require(protectedEngine.StartHandshake().Succeeded());
    Require(protectedSink.actions.front().payload.ReadableBytes() == protectedDatagram.size());
    Require(std::vector<std::uint8_t>(
        protectedSink.actions.front().payload.Peek(),
        protectedSink.actions.front().payload.Peek() + protectedDatagram.size())
        == std::vector<std::uint8_t>(protectedDatagram.begin(), protectedDatagram.end()));

    QuicWireEngine oversizedEngine(options);
    std::vector<std::uint8_t> oversized(options.maximumDatagramBytes + 1, 0xA5);
    Require(oversizedEngine.QueueProtectedDatagram(
        MakeBuffer(std::span<const std::uint8_t>(oversized.data(), oversized.size()))).error
        == static_cast<int>(QuicWireEngineError::DatagramTooLarge));

    QuicEngineOptions serverOptions;
    serverOptions.role = QuicRole::Server;
    QuicWireEngine server(serverOptions);
    Sink serverSink;
    server.SetActionSink(&serverSink);
    Require(server.StartHandshake().Succeeded());
    const auto peerPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 0 },
        BuildQuicCryptoFrame({ 0, tlsBytes }).Value(),
        {}
    });
    Require(peerPacket.IsOk());
    Require(server.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 443),
        MakeBuffer(peerPacket.Value())).Succeeded());
    Require(server.Snapshot().receivedDatagrams == 1);
    Require(server.Snapshot().pathState == QuicPathState::Validating);

    // A server must be able to hand the next caller-protected TLS datagram
    // back to the Engine after it has received the client Initial.
    const std::array<std::uint8_t, 4> serverHandshakeDatagram{
        0xE0, 0x01, 0x02, 0x03 };
    const auto serverHandshake = server.QueueProtectedDatagram(
        MakeBuffer(serverHandshakeDatagram));
    Require(serverHandshake.Succeeded());
    Require(serverHandshake.HasAction(QuicAction::DatagramReady));
    Require(serverSink.actions.back().kind == QuicActionKind::DatagramReady);
    Require(serverSink.actions.back().peer.Ip() == "127.0.0.1"
        && serverSink.actions.back().peer.Port() == 443);
    Require(serverSink.actions.back().payload.ReadableBytes()
        == serverHandshakeDatagram.size());

    QuicWireEngine protectedServer(serverOptions);
    Sink protectedServerSink;
    Observer protectedServerObserver;
    protectedServer.SetActionSink(&protectedServerSink);
    protectedServer.SetEventObserver(&protectedServerObserver);
    protectedServer.SetPacketProtectionProvider(&provider);
    Require(protectedServer.StartHandshake().Succeeded());
    const auto protectedCrypto = BuildQuicCryptoFrame({ 8, tlsBytes });
    Require(protectedCrypto.IsOk());
    const std::array<std::uint8_t, 1> protectionAssociatedData{ 0xC0 };
    std::vector<std::uint8_t> protectedPayload(protectedCrypto.Value().size() + 1);
    const auto protectedPeerResult = provider.Protect({
        QuicPacketProtectionLevel::Initial,
        1,
        protectionAssociatedData,
        protectedCrypto.Value()
    }, protectedPayload);
    Require(protectedPeerResult.Succeeded());
    protectedPayload.resize(protectedPeerResult.bytesWritten);
    const auto protectedPeerPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 1 },
        protectedPayload,
        {}
    });
    Require(protectedPeerPacket.IsOk());
    Require(protectedServer.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 444),
        MakeBuffer(protectedPeerPacket.Value())).Succeeded());
    Require(protectedServer.Snapshot().receivedDatagrams == 1);
    Require(protectedServer.Snapshot().pathState == QuicPathState::Validating);
    Require(protectedServerObserver.events.size() == 1);
    Require(protectedServerObserver.events.front().kind == QuicEventKind::CryptoData);
    Require(protectedServerObserver.events.front().streamId == 8);
    Require(protectedServerObserver.events.front().payload.ReadableBytes() == tlsBytes.size());
    Require(std::vector<std::uint8_t>(
        protectedServerObserver.events.front().payload.Peek(),
        protectedServerObserver.events.front().payload.Peek() + tlsBytes.size())
        == std::vector<std::uint8_t>(tlsBytes.begin(), tlsBytes.end()));

    std::vector<std::uint8_t> protectedFrameSequence{ 0x00 };
    const auto longPing = BuildQuicPingFrame();
    const auto longCrypto = BuildQuicCryptoFrame({ 16, tlsBytes });
    Require(longPing.IsOk() && longCrypto.IsOk());
    protectedFrameSequence.insert(
        protectedFrameSequence.end(), longPing.Value().begin(), longPing.Value().end());
    protectedFrameSequence.insert(
        protectedFrameSequence.end(), longCrypto.Value().begin(), longCrypto.Value().end());
    std::vector<std::uint8_t> protectedSequencePayload(
        protectedFrameSequence.size() + 1);
    const auto protectedSequenceResult = provider.Protect({
        QuicPacketProtectionLevel::Initial,
        2,
        protectionAssociatedData,
        protectedFrameSequence
    }, protectedSequencePayload);
    Require(protectedSequenceResult.Succeeded());
    protectedSequencePayload.resize(protectedSequenceResult.bytesWritten);
    const auto protectedSequencePacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::Initial,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 2 },
        protectedSequencePayload,
        {}
    });
    Require(protectedSequencePacket.IsOk());
    const auto longSequenceResult = protectedServer.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 444),
        MakeBuffer(protectedSequencePacket.Value()));
    Require(longSequenceResult.Succeeded());
    Require(longSequenceResult.HasAction(QuicAction::StreamEvent));
    Require(protectedServerObserver.events.size() == 2);
    Require(protectedServerObserver.events.back().kind == QuicEventKind::CryptoData);
    Require(protectedServerObserver.events.back().streamId == 16);

    QuicWireEngine packetNumberServer(serverOptions);
    Sink packetNumberSink;
    packetNumberServer.SetActionSink(&packetNumberSink);
    packetNumberServer.SetPacketProtectionProvider(&provider);
    Require(packetNumberServer.StartHandshake().Succeeded());
    const auto packetNumberCrypto = BuildQuicCryptoFrame({ 24, tlsBytes });
    Require(packetNumberCrypto.IsOk());
    const auto protectPacketNumber = [&](std::uint8_t packetNumber,
        QuicPacketProtectionLevel level) {
        std::vector<std::uint8_t> payload(packetNumberCrypto.Value().size() + 1);
        const auto protectedResult = provider.Protect({
            level,
            packetNumber,
            protectionAssociatedData,
            packetNumberCrypto.Value()
        }, payload);
        Require(protectedResult.Succeeded());
        payload.resize(protectedResult.bytesWritten);
        const auto packet = BuildQuicLongHeaderPacket({
            level == QuicPacketProtectionLevel::Handshake
                ? QuicLongPacketType::Handshake : QuicLongPacketType::Initial,
            1,
            {},
            {},
            {},
            std::array<std::uint8_t, 1>{ packetNumber },
            payload,
            {}
        });
        Require(packet.IsOk());
        Require(packetNumberServer.ConsumeDatagram(
            LikesProgram::Net::Address("127.0.0.1", 445),
            MakeBuffer(packet.Value())).Succeeded());
    };
    protectPacketNumber(0xFE, QuicPacketProtectionLevel::Initial);
    protectPacketNumber(0x02, QuicPacketProtectionLevel::Initial);
    protectPacketNumber(0x01, QuicPacketProtectionLevel::Handshake);
    const auto zeroRttStream = BuildQuicStreamFrame({
        3, 0, false, true, false, std::array<std::uint8_t, 2>{ 0x41, 0x42 } });
    Require(zeroRttStream.IsOk());
    std::vector<std::uint8_t> zeroRttFrames{ 0x00 };
    const auto zeroRttPing = BuildQuicPingFrame();
    Require(zeroRttPing.IsOk());
    zeroRttFrames.insert(
        zeroRttFrames.end(), zeroRttPing.Value().begin(), zeroRttPing.Value().end());
    zeroRttFrames.insert(
        zeroRttFrames.end(), zeroRttStream.Value().begin(), zeroRttStream.Value().end());
    std::vector<std::uint8_t> zeroRttPayload(zeroRttFrames.size() + 1);
    const auto zeroRttProtected = provider.Protect({
        QuicPacketProtectionLevel::ZeroRtt,
        3,
        protectionAssociatedData,
        zeroRttFrames
    }, zeroRttPayload);
    Require(zeroRttProtected.Succeeded());
    zeroRttPayload.resize(zeroRttProtected.bytesWritten);
    const auto zeroRttPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::ZeroRtt,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 3 },
        zeroRttPayload,
        {}
    });
    Require(zeroRttPacket.IsOk());
    Require(packetNumberServer.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 445),
        MakeBuffer(zeroRttPacket.Value())).Succeeded());
    const auto numberSnapshot = packetNumberServer.Snapshot();
    Require(numberSnapshot.hasInitialPacketNumber
        && numberSnapshot.largestInitialPacketNumber == 0x102);
    Require(numberSnapshot.hasHandshakePacketNumber
        && numberSnapshot.largestHandshakePacketNumber == 1);
    Require(numberSnapshot.hasZeroRttPacketNumber
        && numberSnapshot.largestZeroRttPacketNumber == 3);

    QuicWireEngine zeroRttRejected(serverOptions);
    Sink zeroRttRejectedSink;
    zeroRttRejected.SetActionSink(&zeroRttRejectedSink);
    zeroRttRejected.SetPacketProtectionProvider(&provider);
    Require(zeroRttRejected.StartHandshake().Succeeded());
    const auto forbiddenAck = BuildQuicAckFrame({
        false, 1, 0, { { 1, 1 } }, 0, 0, 0, 0 });
    Require(forbiddenAck.IsOk());
    std::vector<std::uint8_t> forbiddenPayload(forbiddenAck.Value().size() + 1);
    const auto forbiddenProtected = provider.Protect({
        QuicPacketProtectionLevel::ZeroRtt,
        1,
        protectionAssociatedData,
        forbiddenAck.Value()
    }, forbiddenPayload);
    Require(forbiddenProtected.Succeeded());
    forbiddenPayload.resize(forbiddenProtected.bytesWritten);
    const auto forbiddenPacket = BuildQuicLongHeaderPacket({
        QuicLongPacketType::ZeroRtt,
        1,
        {},
        {},
        {},
        std::array<std::uint8_t, 1>{ 1 },
        forbiddenPayload,
        {}
    });
    Require(forbiddenPacket.IsOk());
    Require(!zeroRttRejected.ConsumeDatagram(
        LikesProgram::Net::Address("127.0.0.1", 446),
        MakeBuffer(forbiddenPacket.Value())).Succeeded());
    Require(!zeroRttRejected.Snapshot().hasZeroRttPacketNumber);
}
