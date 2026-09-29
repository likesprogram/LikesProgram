#include <LikesProgram/Quic/Quic.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
    class ConsumerHeaderProtection final
        : public LikesProgram::Quic::QuicPacketHeaderProtectionProvider {
    public:
        LikesProgram::Quic::QuicShortHeaderProtectionResult ProtectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

        LikesProgram::Quic::QuicShortHeaderProtectionResult UnprotectShortHeader(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            return Transform(request, output);
        }

    private:
        static LikesProgram::Quic::QuicShortHeaderProtectionResult Transform(
            const LikesProgram::Quic::QuicShortHeaderProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept {
            const auto validation =
                LikesProgram::Quic::ValidateQuicShortHeaderProtectionRequest(
                    request,
                    std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != LikesProgram::Quic::QuicPacketHeaderProtectionError::None) {
                return { validation, 0, 0, 0 };
            }
            std::copy(request.packet.begin(), request.packet.end(), output.begin());
            return { LikesProgram::Quic::QuicPacketHeaderProtectionError::None,
                request.packet.size(), 7, 1 };
        }
    };

    class ConsumerQuicEngine final : public LikesProgram::Quic::QuicEngine {
    public:
        void SetActionSink(LikesProgram::Quic::QuicActionSink*) noexcept override {}
        void SetEventObserver(LikesProgram::Quic::QuicEventObserver*) noexcept override {}
        void SetPacketProtectionProvider(
            LikesProgram::Quic::QuicPacketProtectionProvider*) noexcept override {}
        LikesProgram::Quic::QuicPacketProtectionResult ProtectPacket(
            const LikesProgram::Quic::QuicPacketProtectionRequest&,
            std::span<std::uint8_t>) noexcept override {
            return { LikesProgram::Quic::QuicPacketProtectionError::NotReady, 0 };
        }
        LikesProgram::Quic::QuicPacketProtectionResult UnprotectPacket(
            const LikesProgram::Quic::QuicPacketProtectionRequest&,
            std::span<std::uint8_t>) noexcept override {
            return { LikesProgram::Quic::QuicPacketProtectionError::NotReady, 0 };
        }

        LikesProgram::Quic::QuicResult StartHandshake() override {
            m_state = LikesProgram::Quic::QuicState::Active;
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        LikesProgram::Quic::QuicResult ProvideTlsHandshakeResult(
            const LikesProgram::Quic::QuicTlsHandshakeResult& result) override {
            const LikesProgram::Quic::QuicEngineOptions options;
            const auto validation = LikesProgram::Quic::ValidateQuicTlsHandshakeResult(
                options, result);
            if (validation != LikesProgram::Quic::QuicTlsHandshakeError::None) {
                return { LikesProgram::Quic::QuicAction::ConnectionClose,
                    static_cast<int>(validation), {} };
            }
            if (result.state == LikesProgram::Quic::QuicTlsHandshakeState::Failed) {
                return { LikesProgram::Quic::QuicAction::ConnectionClose, result.error, {} };
            }
            if (result.state == LikesProgram::Quic::QuicTlsHandshakeState::Complete) {
                m_state = LikesProgram::Quic::QuicState::Active;
                m_packetProtectionReady = result.packetProtectionReady;
            }
            return {};
        }

        LikesProgram::Quic::QuicResult ConsumeDatagram(
            const LikesProgram::Net::Address&,
            LikesProgram::Net::Buffer&& datagram) override {
            datagram.RetrieveAll();
            return {};
        }

        LikesProgram::Quic::QuicResult HandleTimeout() override { return {}; }

        LikesProgram::Quic::QuicResult SendStreamData(
            std::uint64_t,
            LikesProgram::Net::Buffer&& plaintext) override {
            plaintext.RetrieveAll();
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        LikesProgram::Quic::QuicResult SendStreamFin(std::uint64_t) override { return {}; }
        LikesProgram::Quic::QuicResult ResetStream(std::uint64_t, std::uint64_t) override { return {}; }
        LikesProgram::Quic::QuicResult StopSending(std::uint64_t, std::uint64_t) override { return {}; }

        LikesProgram::Quic::QuicResult Close(std::uint64_t) override {
            m_state = LikesProgram::Quic::QuicState::Closed;
            return { LikesProgram::Quic::QuicAction::ConnectionClose, 0, {} };
        }

        LikesProgram::Quic::QuicState State() const noexcept override { return m_state; }

        LikesProgram::Quic::QuicEngineSnapshot Snapshot() const noexcept override {
            return {
                m_state,
                LikesProgram::Quic::QuicTlsVersion::Tls13,
                LikesProgram::Quic::QuicPathState::Validated,
                m_packetProtectionReady,
                m_state == LikesProgram::Quic::QuicState::Active,
                0,
                0,
                0,
                0,
                0
            };
        }

        const char* NegotiatedProtocol() const noexcept override { return "h3"; }

        LikesProgram::Quic::QuicTlsVersion NegotiatedTlsVersion() const noexcept override {
            return LikesProgram::Quic::QuicTlsVersion::Tls13;
        }

    private:
        LikesProgram::Quic::QuicState m_state = LikesProgram::Quic::QuicState::Handshaking;
        bool m_packetProtectionReady = false;
    };
}

int main() {
    if (!LikesProgram::Quic::PackageAvailable()
        || std::string_view(LikesProgram::Quic::PackageName()) != "LikesProgramQuic") {
        return 1;
    }

    const auto encoded = LikesProgram::Quic::EncodeQuicVarInt(15'293);
    if (!encoded.IsOk()) return 2;
    const auto decoded = LikesProgram::Quic::ParseQuicVarInt(
        encoded.Value().Data(), encoded.Value().size);
    if (!decoded.IsOk()
        || decoded.Value().value != 15'293
        || decoded.Value().encodedBytes != 2) return 2;

    const std::array<std::uint8_t, 1> destinationId{ 0x01 };
    const std::array<std::uint8_t, 1> sourceId{ 0x02 };
    const std::array<std::uint8_t, 1> packetNumberBytes{ 0x03 };
    const std::array<std::uint8_t, 1> payload{ 0x04 };
    LikesProgram::Quic::QuicLongHeaderBuildOptions header;
    header.destinationConnectionId = destinationId;
    header.sourceConnectionId = sourceId;
    header.packetNumber = packetNumberBytes;
    header.payload = payload;
    const auto packet = LikesProgram::Quic::BuildQuicLongHeaderPacket(header);
    if (!packet.IsOk()) return 3;
    const auto parsedHeader = LikesProgram::Quic::ParseQuicLongHeader(
        packet.Value().data(), packet.Value().size());
    if (!parsedHeader.IsOk()
        || parsedHeader.Value().type != LikesProgram::Quic::QuicLongPacketType::Initial
        || parsedHeader.Value().payload.size() != 1) return 3;
    const auto packetNumberEncoding = LikesProgram::Quic::EncodeQuicPacketNumber(
        0xACE8FE, std::optional<std::uint64_t>{ 0xABE8B3 });
    if (!packetNumberEncoding.IsOk() || packetNumberEncoding.Value().size != 3) return 3;
    if (LikesProgram::Quic::DecodeQuicPacketNumber(
            0xA82F30EA, 0x9B32, 2).Value() != 0xA82F9B32) return 3;
    LikesProgram::Quic::QuicPacketNumberSpace packetNumberSpace;
    const auto firstPacket = packetNumberSpace.Decode(0xFE, 1);
    const auto wrappedPacket = packetNumberSpace.Decode(0x02, 1);
    if (!firstPacket.IsOk() || !wrappedPacket.IsOk()
        || wrappedPacket.Value() != 0x102
        || packetNumberSpace.Snapshot().largestReceived != 0x102) return 3;
    LikesProgram::Quic::QuicAckFrame ack;
    ack.largestAcknowledged = 4;
    ack.ranges = { { 3, 4 } };
    if (!LikesProgram::Quic::BuildQuicAckFrame(ack).IsOk()) return 3;
    LikesProgram::Quic::QuicAckObservationState ackObservation;
    ack.ecn = true;
    ack.ect0Count = 2;
    if (!LikesProgram::Quic::ObserveQuicAck(ackObservation, ack).Succeeded()
        || ackObservation.ect0Observed != 2) return 3;
    LikesProgram::Quic::QuicRetransmissionQueue retransmission({ 4, 16 });
    const auto expiry = std::chrono::steady_clock::time_point{};
    if (!retransmission.Track(3, { 1 }, expiry).IsOk()
        || !retransmission.Track(4, { 2 }, expiry).IsOk()) return 3;
    const auto appliedAck = LikesProgram::Quic::ApplyQuicAckFrame(
        retransmission, ack);
    if (!appliedAck.IsOk()
        || appliedAck.Value().acknowledgedPackets != 2
        || appliedAck.Value().untrackedPackets != 0) return 3;
    LikesProgram::Quic::QuicAckDelayContext ackDelay;
    if (LikesProgram::Quic::ConfigureQuicAckDelay(ackDelay, 3, 80)
            != LikesProgram::Quic::QuicAckDelayError::None
        || !LikesProgram::Quic::DecodeQuicAckDelay(ackDelay, 10).Succeeded()
        || !LikesProgram::Quic::ObserveQuicAck(ackObservation, ack, ackDelay).Succeeded()) return 3;
    const std::array<std::uint8_t, 1> streamPayload{ 0x05 };
    LikesProgram::Quic::QuicStreamFrameBuildOptions stream;
    stream.streamId = 0;
    stream.data = streamPayload;
    if (!LikesProgram::Quic::BuildQuicStreamFrame(stream).IsOk()) return 3;
    LikesProgram::Quic::QuicCryptoFrameBuildOptions crypto;
    crypto.data = streamPayload;
    if (!LikesProgram::Quic::BuildQuicCryptoFrame(crypto).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicResetStreamFrame(
            LikesProgram::Quic::QuicResetStreamFrame{ 0, 1, 1, 0 }).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicStopSendingFrame(
            LikesProgram::Quic::QuicStopSendingFrame{ 0, 1, 0 }).IsOk()) return 3;
    LikesProgram::Quic::QuicConnectionCloseFrame close;
    close.application = true;
    close.errorCode = 1;
    if (!LikesProgram::Quic::BuildQuicConnectionCloseFrame(close).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicFlowControlFrame({
            LikesProgram::Quic::QuicFlowControlFrameKind::MaxData, 0, 1024, 0 }).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicPathValidationFrame({
            LikesProgram::Quic::QuicPathValidationFrameKind::PathChallenge,
            { 0, 1, 2, 3, 4, 5, 6, 7 }, 0 }).IsOk()) return 3;
    const auto pathResponse = LikesProgram::Quic::BuildQuicPathResponseFrame({
        LikesProgram::Quic::QuicPathValidationFrameKind::PathChallenge,
        { 0, 1, 2, 3, 4, 5, 6, 7 }, 0 });
    if (!pathResponse.IsOk()
        || pathResponse.Value().kind
            != LikesProgram::Quic::QuicPathValidationFrameKind::PathResponse) return 3;
    if (!LikesProgram::Quic::BuildQuicConnectionIdFrame({
            LikesProgram::Quic::QuicConnectionIdFrameKind::RetireConnectionId,
            1, 0, {}, {}, 0 }).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicPingFrame().IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicNewTokenFrame({ streamPayload }).IsOk()) return 3;
    if (!LikesProgram::Quic::BuildQuicHandshakeDoneFrame().IsOk()) return 3;
    const auto datagram = LikesProgram::Quic::BuildQuicDatagramFrame(
        true, streamPayload);
    if (!datagram.IsOk()
        || !LikesProgram::Quic::ParseQuicDatagramFrame(
            datagram.Value().data(), datagram.Value().size()).IsOk()) return 3;

    const std::array<std::uint8_t, 4> shortPacket{ 0x41, 0x01, 0x02, 0x03 };
    const LikesProgram::Quic::QuicShortHeaderProtectionRequest protectionRequest{
        LikesProgram::Quic::QuicPacketProtectionLevel::OneRtt, 6, 1, shortPacket };
    std::array<std::uint8_t, 4> unprotectedOutput{};
    ConsumerHeaderProtection headerProtection;
    const auto unprotected = headerProtection.UnprotectShortHeader(
        protectionRequest, unprotectedOutput);
    if (!unprotected.Succeeded()
        || LikesProgram::Quic::ValidateQuicShortHeaderProtectionResult(
                protectionRequest, unprotected, unprotectedOutput)
            != LikesProgram::Quic::QuicPacketHeaderProtectionError::None
        || unprotected.packetNumber != 7
        || unprotected.packetNumberLength != 1) return 3;

    LikesProgram::Quic::QuicEngineFactory factory(
        [](const LikesProgram::Quic::QuicEngineOptions&) {
            return std::make_unique<ConsumerQuicEngine>();
        });
    if (!factory) return 4;

    LikesProgram::Quic::QuicEngineOptions options;
    auto engine = factory.Create(options);
    if (!engine
        || !engine->StartHandshake().Succeeded()
        || !engine->ProvideTlsHandshakeResult({
            LikesProgram::Quic::QuicTlsHandshakeState::Complete,
            LikesProgram::Quic::QuicTlsVersion::Tls13,
            "h3",
            true,
            true,
            0
        }).Succeeded()
        || engine->NegotiatedTlsVersion() != LikesProgram::Quic::QuicTlsVersion::Tls13
        || std::string_view(engine->NegotiatedProtocol()) != "h3") {
        return 5;
    }

    engine->SendStreamData(0, LikesProgram::Net::Buffer(0));
    engine->SendStreamFin(0);
    engine->ResetStream(0, 1);
    engine->StopSending(0, 1);
    engine->Close(0);
    if (engine->State() != LikesProgram::Quic::QuicState::Closed) return 6;

    std::cout << LikesProgram::Quic::PackageName() << " consumer check passed\n";
    return 0;
}
