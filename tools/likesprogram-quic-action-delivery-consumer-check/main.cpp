#include <LikesProgram/Quic/QuicEngineFactory.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using LikesProgram::Net::Buffer;
    using LikesProgram::Quic::QuicActionDelivery;
    using LikesProgram::Quic::QuicActionDeliveryError;
    using LikesProgram::Quic::QuicActionDeliveryState;
    using LikesProgram::Quic::QuicActionKind;
    using LikesProgram::Quic::QuicActionMessage;
    using LikesProgram::Quic::QuicActionSink;
    using LikesProgram::Quic::QuicEngine;
    using LikesProgram::Quic::QuicEngineOptions;
    using LikesProgram::Quic::QuicResult;
    using LikesProgram::Quic::QuicState;
    using LikesProgram::Quic::QuicTlsHandshakeResult;

    Buffer MakeBuffer(std::string_view value) {
        Buffer buffer(0);
        buffer.Append(value.data(), value.size());
        return buffer;
    }

    QuicActionMessage CopyAction(const QuicActionMessage& source,
        std::uint64_t actionId) {
        QuicActionMessage copy;
        copy.kind = source.kind;
        copy.actionId = actionId;
        copy.streamId = source.streamId;
        copy.errorCode = source.errorCode;
        copy.timerDelay = source.timerDelay;
        copy.peer = source.peer;
        copy.payload = MakeBuffer(source.payload.AsStringView());
        return copy;
    }

    class DeliveryRecorder final : public QuicActionSink {
    public:
        QuicActionDelivery Submit(QuicActionMessage&& action) noexcept override {
            if (action.actionId == 0) {
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::InvalidAction, 0 };
            }
            try {
                received.push_back(std::move(action));
            }
            catch (...) {
                failed = true;
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::InvalidAction, action.actionId };
            }

            auto delivery = nextDelivery;
            nextDelivery = {};
            if (delivery.actionId == 0) {
                delivery.actionId = received.back().actionId;
            }
            lastDelivery = delivery;
            return delivery;
        }

        std::vector<QuicActionMessage> received;
        QuicActionDelivery nextDelivery{};
        QuicActionDelivery lastDelivery{};
        bool failed = false;
    };

    class DeliveryEngine final : public QuicEngine {
    public:
        void SetActionSink(QuicActionSink* sink) noexcept override {
            m_sink = sink;
        }

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

        QuicResult StartHandshake() override {
            if (m_state != QuicState::Handshaking) return Failed(1);
            const auto delivery = Emit(QuicActionMessage(
                QuicActionKind::DatagramReady, 0, 0));
            if (delivery.Rejected() || !delivery.Matches(m_lastActionId)) {
                m_state = QuicState::Failed;
                return Failed(static_cast<int>(delivery.error));
            }
            m_state = QuicState::Active;
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        QuicResult ProvideTlsHandshakeResult(
            const QuicTlsHandshakeResult&) override {
            return {};
        }

        QuicResult ConsumeDatagram(const LikesProgram::Net::Address&,
            Buffer&& datagram) override {
            datagram.RetrieveAll();
            return {};
        }

        QuicResult HandleTimeout() override { return {}; }

        QuicResult SendStreamData(std::uint64_t streamId,
            Buffer&& plaintext) override {
            QuicActionMessage action(QuicActionKind::DatagramReady, streamId);
            action.payload = std::move(plaintext);
            return EmitResult(action);
        }

        QuicResult SendStreamFin(std::uint64_t streamId) override {
            return EmitResult(QuicActionMessage(
                QuicActionKind::DatagramReady, streamId));
        }

        QuicResult ResetStream(std::uint64_t streamId,
            std::uint64_t errorCode) override {
            return EmitResult(QuicActionMessage(
                QuicActionKind::ResetStream, streamId, errorCode));
        }

        QuicResult StopSending(std::uint64_t streamId,
            std::uint64_t errorCode) override {
            return EmitResult(QuicActionMessage(
                QuicActionKind::StopSending, streamId, errorCode));
        }

        QuicResult Close(std::uint64_t errorCode) override {
            const auto result = EmitResult(QuicActionMessage(
                QuicActionKind::CloseConnection, 0, errorCode));
            if (result.Succeeded()) m_state = QuicState::Closed;
            return result;
        }

        QuicState State() const noexcept override { return m_state; }

        LikesProgram::Quic::QuicEngineSnapshot Snapshot() const noexcept override {
            return { m_state, LikesProgram::Quic::QuicTlsVersion::Unknown,
                LikesProgram::Quic::QuicPathState::Unknown, false, false,
                0, 0, 0, 0, 0 };
        }

        const char* NegotiatedProtocol() const noexcept override { return ""; }

        LikesProgram::Quic::QuicTlsVersion NegotiatedTlsVersion() const noexcept override {
            return LikesProgram::Quic::QuicTlsVersion::Unknown;
        }

        std::uint64_t LastActionId() const noexcept { return m_lastActionId; }

    private:
        static QuicResult Failed(int error) {
            return { LikesProgram::Quic::QuicAction::None, error, {} };
        }

        QuicActionDelivery Emit(QuicActionMessage action) {
            if (m_sink == nullptr) {
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::TransportUnavailable, action.actionId };
            }
            if (action.actionId == 0) action.actionId = m_nextActionId++;
            m_lastActionId = action.actionId;
            const auto delivery = m_sink->Submit(std::move(action));
            if (!delivery.Matches(m_lastActionId)) {
                return { QuicActionDeliveryState::Rejected,
                    QuicActionDeliveryError::InvalidAction, m_lastActionId };
            }
            return delivery;
        }

        QuicResult EmitResult(QuicActionMessage action) {
            const auto delivery = Emit(std::move(action));
            if (delivery.Rejected()) return Failed(static_cast<int>(delivery.error));
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        QuicActionSink* m_sink = nullptr;
        QuicState m_state = QuicState::Handshaking;
        std::uint64_t m_nextActionId = 1;
        std::uint64_t m_lastActionId = 0;
    };

    bool IsDelivery(const QuicActionDelivery& delivery,
        QuicActionDeliveryState state, QuicActionDeliveryError error,
        std::uint64_t actionId) {
        return delivery.state == state && delivery.error == error
            && delivery.actionId == actionId;
    }
}

int main() {
    using namespace LikesProgram::Quic;

    DeliveryRecorder recorder;
    DeliveryRecorder retryRecorder;
    QuicEngineFactory factory([](const QuicEngineOptions&) {
        return std::make_unique<DeliveryEngine>();
    });
    auto engine = factory.Create({});
    if (!engine) return 1;

    if (engine->StartHandshake().Succeeded()) return 2;

    engine = factory.Create({});
    engine->SetActionSink(&recorder);
    if (!engine->StartHandshake().Succeeded()
        || recorder.received.size() != 1
        || recorder.received.back().actionId != 1) return 3;

    const auto handshake = recorder.received.back();
    recorder.nextDelivery = { QuicActionDeliveryState::Deferred,
        QuicActionDeliveryError::Backpressure };
    if (!engine->SendStreamData(4, MakeBuffer("payload")).Succeeded()
        || recorder.received.size() != 2
        || !IsDelivery(recorder.lastDelivery, QuicActionDeliveryState::Deferred,
            QuicActionDeliveryError::Backpressure, 2)
        || recorder.received.back().payload.AsStringView() != "payload") return 4;

    auto retry = CopyAction(recorder.received.back(), 3);
    recorder.nextDelivery = {};
    const auto retryDelivery = retryRecorder.Submit(std::move(retry));
    if (!IsDelivery(retryDelivery, QuicActionDeliveryState::Accepted,
            QuicActionDeliveryError::None, 3)
        || retryRecorder.received.back().actionId != 3
        || retryRecorder.received.back().payload.AsStringView() != "payload") return 5;

    recorder.nextDelivery = { QuicActionDeliveryState::Deferred,
        QuicActionDeliveryError::Backpressure, 9999 };
    if (engine->SendStreamData(4, MakeBuffer("mismatch")).Succeeded()
        || !IsDelivery(recorder.lastDelivery, QuicActionDeliveryState::Deferred,
            QuicActionDeliveryError::Backpressure, 9999)) return 6;

    recorder.nextDelivery = { QuicActionDeliveryState::Rejected,
        QuicActionDeliveryError::TransportUnavailable };
    if (engine->ResetStream(4, 0x31).Succeeded()
        || !IsDelivery(recorder.lastDelivery, QuicActionDeliveryState::Rejected,
            QuicActionDeliveryError::TransportUnavailable, 4)
        || recorder.received.back().kind != QuicActionKind::ResetStream
        || recorder.received.back().streamId != 4
        || recorder.received.back().errorCode != 0x31) return 7;

    recorder.nextDelivery = {};
    auto resetRetry = CopyAction(recorder.received.back(), 6);
    if (!retryRecorder.Submit(std::move(resetRetry)).Accepted()) return 8;

    if (!engine->StopSending(4, 0x32).Succeeded()
        || recorder.received.back().kind != QuicActionKind::StopSending
        || recorder.received.back().actionId != 5
        || recorder.received.back().streamId != 4
        || recorder.received.back().errorCode != 0x32) return 9;
    if (!engine->Close(0x40).Succeeded()
        || engine->State() != QuicState::Closed
        || recorder.received.back().kind != QuicActionKind::CloseConnection
        || recorder.received.back().errorCode != 0x40) return 10;

    const QuicActionMessage invalid;
    const auto invalidDelivery = retryRecorder.Submit(QuicActionMessage(invalid));
    if (!IsDelivery(invalidDelivery, QuicActionDeliveryState::Rejected,
            QuicActionDeliveryError::InvalidAction, 0)) return 11;
    const QuicActionDelivery acceptedWithError{
        QuicActionDeliveryState::Accepted,
        QuicActionDeliveryError::Backpressure, 1 };
    if (acceptedWithError.Accepted() || acceptedWithError.Matches(0)
        || handshake.payload.ReadableBytes() != 0) return 12;
    if (recorder.failed) return 13;

    std::cout << "passed=true"
              << " accepted=5"
              << " deferred=1"
              << " rejected=3"
              << " retry_new_ids=true"
              << " payload_owned=true"
              << " metadata_preserved=true\n";
    return 0;
}
