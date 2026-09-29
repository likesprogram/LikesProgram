#include <LikesProgram/Quic/QuicEngineFactory.hpp>
#include <LikesProgram/Quic/QuicPathValidationFrame.hpp>
#include <LikesProgram/Quic/QuicPathValidationAction.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

void RunQuicVarIntTests();
void RunQuicLongHeaderTests();
void RunQuicPacketNumberTests();
void RunQuicPacketNumberSpaceTests();
void RunQuicShortHeaderTests();
void RunQuicPacketHeaderProtectionTests();
void RunQuicRetransmissionQueueTests();
void RunQuicCongestionBudgetTests();
void RunQuicFrameTypeTests();
void RunQuicAckFrameTests();
void RunQuicAckApplicationTests();
void RunQuicAckDelayTests();
void RunQuicStreamFrameTests();
void RunQuicCryptoFrameTests();
void RunQuicStreamControlFrameTests();
void RunQuicConnectionCloseFrameTests();
void RunQuicConnectionIdFrameTests();
void RunQuicFlowControlFrameTests();
void RunQuicPathValidationFrameTests();
void RunQuicPingFrameTests();
void RunQuicDatagramFrameTests();
void RunQuicNewTokenFrameTests();
void RunQuicHandshakeDoneFrameTests();
void RunQuicTimeoutTests();
void RunQuicStreamCreditTests();
void RunQuicConnectionCreditTests();
void RunQuicCreditReservationTests();
void RunQuicAckTrackerTests();
void RunQuicAckRecoveryLedgerTests();
void RunQuicCreditCongestionLedgerTests();
void RunQuicPathMigrationLedgerTests();
void RunQuicConnectionIdTrackerTests();
void RunQuicPathProbeTrackerTests();
void RunQuicStreamTerminalTrackerTests();
void RunQuicWireEngineTests();

namespace {
    // 契约程序失败时立即终止，保持测试入口依赖最小。
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    // 构造测试 datagram，验证输入所有权在 Engine 边界内转移。
    LikesProgram::Net::Buffer MakeBuffer(std::string_view value) {
        LikesProgram::Net::Buffer buffer(0);
        buffer.Append(value.data(), value.size());
        return buffer;
    }

    class ActionCollector final : public LikesProgram::Quic::QuicActionSink {
    public:
        // 接管一次 action 消息，保存 datagram/stream payload。
        LikesProgram::Quic::QuicActionDelivery Submit(
            LikesProgram::Quic::QuicActionMessage&& action) noexcept override {
            m_actions.emplace_back(std::move(action));
            auto delivery = m_nextDelivery;
            m_nextDelivery = {};
            if (delivery.actionId == 0) {
                delivery.actionId = m_actions.back().actionId;
            }
            m_lastDelivery = delivery;
            return delivery;
        }

        std::vector<LikesProgram::Quic::QuicActionMessage> m_actions; // 测试动作记录
        LikesProgram::Quic::QuicActionDelivery m_nextDelivery{};
        LikesProgram::Quic::QuicActionDelivery m_lastDelivery{};
    };

    class EventCollector final : public LikesProgram::Quic::QuicEventObserver {
    public:
        // 接管一次入站 stream/path 事件。
        void Observe(LikesProgram::Quic::QuicStreamEvent&& event) noexcept override {
            m_events.emplace_back(std::move(event));
        }

        std::vector<LikesProgram::Quic::QuicStreamEvent> m_events; // 测试事件记录
    };

    class FakePacketProtection final
        : public LikesProgram::Quic::QuicPacketProtectionProvider {
    public:
        LikesProgram::Quic::QuicPacketProtectionResult Protect(
            const LikesProgram::Quic::QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            const auto validation = LikesProgram::Quic::ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != LikesProgram::Quic::QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            if (request.payload.size() == 0) return { {}, 0 };
            std::copy(request.payload.begin(), request.payload.end(), output.begin());
            return { {}, request.payload.size() };
        }

        LikesProgram::Quic::QuicPacketProtectionResult Unprotect(
            const LikesProgram::Quic::QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (request.payload.size() > 0 && request.payload.front() == 0xFF) {
                return {
                    LikesProgram::Quic::QuicPacketProtectionError::AuthenticationFailed,
                    0
                };
            }
            const auto validation = LikesProgram::Quic::ValidateQuicPacketProtectionRequest(
                request, std::span<const std::uint8_t>(output.data(), output.size()));
            if (validation != LikesProgram::Quic::QuicPacketProtectionError::None) {
                return { validation, 0 };
            }
            std::copy(request.payload.begin(), request.payload.end(), output.begin());
            return { {}, request.payload.size() };
        }
    };

    class FakeQuicEngine final : public LikesProgram::Quic::QuicEngine {
    public:
        explicit FakeQuicEngine(const LikesProgram::Quic::QuicEngineOptions& options)
            : m_options(options) {
        }

        // 设置非拥有动作输出端。
        void SetActionSink(LikesProgram::Quic::QuicActionSink* sink) noexcept override {
            m_actionSink = sink;
        }

        // 设置非拥有入站事件观察端。
        void SetEventObserver(LikesProgram::Quic::QuicEventObserver* observer) noexcept override {
            m_eventObserver = observer;
        }

        void SetPacketProtectionProvider(
            LikesProgram::Quic::QuicPacketProtectionProvider* provider) noexcept override {
            m_packetProtectionProvider = provider;
        }

        LikesProgram::Quic::QuicPacketProtectionResult ProtectPacket(
            const LikesProgram::Quic::QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (m_packetProtectionProvider == nullptr) {
                return { LikesProgram::Quic::QuicPacketProtectionError::NotReady, 0 };
            }
            return m_packetProtectionProvider->Protect(request, output);
        }

        LikesProgram::Quic::QuicPacketProtectionResult UnprotectPacket(
            const LikesProgram::Quic::QuicPacketProtectionRequest& request,
            std::span<std::uint8_t> output) noexcept override {
            if (m_packetProtectionProvider == nullptr) {
                return { LikesProgram::Quic::QuicPacketProtectionError::NotReady, 0 };
            }
            return m_packetProtectionProvider->Unprotect(request, output);
        }

        // 产生最小握手 datagram，并要求一次 timer。
        LikesProgram::Quic::QuicResult StartHandshake() override {
            if (m_state != LikesProgram::Quic::QuicState::Handshaking) return Failed(1);
            const auto delivery = EmitDatagram("CHLO");
            if (delivery.Rejected()) return Failed(static_cast<int>(delivery.error));
            m_started = true;
            return {
                LikesProgram::Quic::QuicAction::DatagramReady
                    | LikesProgram::Quic::QuicAction::ArmTimer,
                0,
                std::chrono::milliseconds(25)
            };
        }

        // 解释 fake wire marker，覆盖握手、stream、close 和迁移事件。
        LikesProgram::Quic::QuicResult ProvideTlsHandshakeResult(
            const LikesProgram::Quic::QuicTlsHandshakeResult& result) override {
            if (!m_started) return Failed(9);
            const auto validation = LikesProgram::Quic::ValidateQuicTlsHandshakeResult(
                m_options, result);
            if (validation != LikesProgram::Quic::QuicTlsHandshakeError::None) {
                return Failed(static_cast<int>(validation));
            }
            if (result.state == LikesProgram::Quic::QuicTlsHandshakeState::Failed) {
                return Failed(result.error);
            }
            if (result.state == LikesProgram::Quic::QuicTlsHandshakeState::InProgress) {
                return {};
            }
            if (m_state != LikesProgram::Quic::QuicState::Handshaking) return Failed(10);
            m_state = LikesProgram::Quic::QuicState::Active;
            m_tlsVersion = result.tlsVersion;
            m_negotiatedProtocol.assign(result.applicationProtocol);
            m_packetProtectionReady = result.packetProtectionReady;
            EmitEvent(LikesProgram::Quic::QuicEventKind::HandshakeComplete);
            EmitAction(LikesProgram::Quic::QuicActionMessage{
                LikesProgram::Quic::QuicActionKind::CancelTimer
            });
            return {
                LikesProgram::Quic::QuicAction::StreamEvent,
                0,
                std::chrono::milliseconds(0)
            };
        }

        LikesProgram::Quic::QuicResult ConsumeDatagram(
            const LikesProgram::Net::Address& peer,
            LikesProgram::Net::Buffer&& datagram) override {
            const std::string_view marker = datagram.AsStringView();
            datagram.RetrieveAll();
            ++m_receivedDatagrams;
            m_peer = peer;

            if (!m_started) return Failed(2);
            if (m_state == LikesProgram::Quic::QuicState::Handshaking
                && marker == "SHLO") {
                const auto handshake = ProvideTlsHandshakeResult({
                    LikesProgram::Quic::QuicTlsHandshakeState::Complete,
                    LikesProgram::Quic::QuicTlsVersion::Tls13,
                    "h3",
                    true,
                    true,
                    0
                });
                if (!handshake.Succeeded()) return handshake;
                m_pathState = LikesProgram::Quic::QuicPathState::Validated;
                EmitEvent(LikesProgram::Quic::QuicEventKind::PathValidated);
                return {
                    handshake.actions | LikesProgram::Quic::QuicAction::PathEvent,
                    0,
                    std::chrono::milliseconds(0)
                };
            }
            if (m_state != LikesProgram::Quic::QuicState::Active) return Failed(3);
            if (marker == "MIGRATE") {
                m_pathState = LikesProgram::Quic::QuicPathState::Migrated;
                EmitEvent(LikesProgram::Quic::QuicEventKind::PathMigrated);
                return { LikesProgram::Quic::QuicAction::PathEvent, 0, {} };
            }
            if (marker == "CLOSE") {
                m_state = LikesProgram::Quic::QuicState::Closed;
                EmitEvent(LikesProgram::Quic::QuicEventKind::ConnectionClose);
                return { LikesProgram::Quic::QuicAction::ConnectionClose, 0, {} };
            }
            if (marker == "STREAM") {
                EmitEvent(
                    LikesProgram::Quic::QuicEventKind::StreamData,
                    4,
                    MakeBuffer("payload"));
                return { LikesProgram::Quic::QuicAction::StreamEvent, 0, {} };
            }
            if (marker == "FIN") {
                EmitEvent(LikesProgram::Quic::QuicEventKind::StreamFin, 4);
                return { LikesProgram::Quic::QuicAction::StreamEvent, 0, {} };
            }
            if (marker == "RESET") {
                EmitEvent(
                    LikesProgram::Quic::QuicEventKind::StreamReset,
                    4,
                    LikesProgram::Net::Buffer(0),
                    7);
                return { LikesProgram::Quic::QuicAction::StreamEvent, 0, {} };
            }
            return Failed(4);
        }

        // 握手未完成时 timer 进入失败态，避免永久 Handshaking。
        LikesProgram::Quic::QuicResult HandleTimeout() override {
            if (m_state != LikesProgram::Quic::QuicState::Handshaking) return {};
            m_state = LikesProgram::Quic::QuicState::Failed;
            m_lastError = 110;
            return Failed(110);
        }

        // 将应用 stream data 包装为 fake datagram action。
        LikesProgram::Quic::QuicResult SendStreamData(
            std::uint64_t streamId,
            LikesProgram::Net::Buffer&& plaintext) override {
            if (m_state != LikesProgram::Quic::QuicState::Active) return Failed(5);
            plaintext.RetrieveAll();
            ++m_activeStreams;
            const auto delivery = EmitDatagram("STREAMDATA", streamId);
            if (delivery.Rejected()) return Failed(static_cast<int>(delivery.error));
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        // 发送 stream FIN action。
        LikesProgram::Quic::QuicResult SendStreamFin(std::uint64_t streamId) override {
            if (m_state != LikesProgram::Quic::QuicState::Active) return Failed(6);
            EmitAction(LikesProgram::Quic::QuicActionMessage{
                LikesProgram::Quic::QuicActionKind::DatagramReady,
                streamId
            });
            return { LikesProgram::Quic::QuicAction::DatagramReady, 0, {} };
        }

        // 发送 RESET_STREAM action。
        LikesProgram::Quic::QuicResult ResetStream(
            std::uint64_t streamId,
            std::uint64_t errorCode) override {
            if (m_state != LikesProgram::Quic::QuicState::Active) return Failed(7);
            EmitAction(LikesProgram::Quic::QuicActionMessage{
                LikesProgram::Quic::QuicActionKind::ResetStream,
                streamId,
                errorCode
            });
            return { LikesProgram::Quic::QuicAction::StreamEvent, 0, {} };
        }

        // 发送 STOP_SENDING action。
        LikesProgram::Quic::QuicResult StopSending(
            std::uint64_t streamId,
            std::uint64_t errorCode) override {
            if (m_state != LikesProgram::Quic::QuicState::Active) return Failed(8);
            EmitAction(LikesProgram::Quic::QuicActionMessage{
                LikesProgram::Quic::QuicActionKind::StopSending,
                streamId,
                errorCode
            });
            return { LikesProgram::Quic::QuicAction::StreamEvent, 0, {} };
        }

        // 进入关闭态并产生 connection close action。
        LikesProgram::Quic::QuicResult Close(std::uint64_t errorCode) override {
            if (m_state == LikesProgram::Quic::QuicState::Closed) return {};
            m_state = LikesProgram::Quic::QuicState::Closed;
            EmitAction(LikesProgram::Quic::QuicActionMessage{
                LikesProgram::Quic::QuicActionKind::CloseConnection,
                0,
                errorCode
            });
            return { LikesProgram::Quic::QuicAction::ConnectionClose, 0, {} };
        }

        // 返回当前 fake Engine 状态。
        LikesProgram::Quic::QuicState State() const noexcept override {
            return m_state;
        }

        // 返回当前 fake Engine 快照。
        LikesProgram::Quic::QuicEngineSnapshot Snapshot() const noexcept override {
            return {
                m_state,
                m_tlsVersion,
                m_pathState,
                m_state == LikesProgram::Quic::QuicState::Active,
                m_packetProtectionReady,
                m_activeStreams,
                0,
                m_receivedDatagrams,
                m_sentDatagrams,
                m_lastError
            };
        }

        // 握手成功后返回 h3 ALPN。
        const char* NegotiatedProtocol() const noexcept override {
            return m_state == LikesProgram::Quic::QuicState::Active
                ? m_negotiatedProtocol.c_str() : "";
        }

        // 握手成功后报告 TLS 1.3。
        LikesProgram::Quic::QuicTlsVersion NegotiatedTlsVersion() const noexcept override {
            return m_tlsVersion;
        }

    private:
        // 构造失败结果并保存稳定错误码。
        LikesProgram::Quic::QuicResult Failed(int error) {
            m_state = LikesProgram::Quic::QuicState::Failed;
            m_lastError = error;
            return { LikesProgram::Quic::QuicAction::ConnectionClose, error, {} };
        }

        // 向动作端提交一个 fake datagram。
        LikesProgram::Quic::QuicActionDelivery EmitDatagram(
            std::string_view marker,
            std::uint64_t streamId = 0) {
            LikesProgram::Quic::QuicActionMessage action;
            action.kind = LikesProgram::Quic::QuicActionKind::DatagramReady;
            action.actionId = m_nextActionId++;
            action.streamId = streamId;
            action.payload = MakeBuffer(marker);
            const auto delivery = EmitAction(std::move(action));
            ++m_sentDatagrams;
            return delivery;
        }

        // 向事件端提交一个入站事件。
        void EmitEvent(
            LikesProgram::Quic::QuicEventKind kind,
            std::uint64_t streamId = 0,
            LikesProgram::Net::Buffer payload = LikesProgram::Net::Buffer(0),
            std::uint64_t errorCode = 0) {
            if (m_eventObserver == nullptr) return;
            LikesProgram::Quic::QuicStreamEvent event;
            event.kind = kind;
            event.streamId = streamId;
            event.errorCode = errorCode;
            event.peer = m_peer;
            event.payload = std::move(payload);
            m_eventObserver->Observe(std::move(event));
        }

        // 向动作端提交一个控制动作。
        LikesProgram::Quic::QuicActionDelivery EmitAction(
            LikesProgram::Quic::QuicActionMessage action) {
            if (m_actionSink == nullptr) {
                return {
                    LikesProgram::Quic::QuicActionDeliveryState::Rejected,
                    LikesProgram::Quic::QuicActionDeliveryError::TransportUnavailable,
                    action.actionId
                };
            }
            if (action.actionId == 0) action.actionId = m_nextActionId++;
            const auto delivery = m_actionSink->Submit(std::move(action));
            if (!delivery.Matches(action.actionId)) {
                return {
                    LikesProgram::Quic::QuicActionDeliveryState::Rejected,
                    LikesProgram::Quic::QuicActionDeliveryError::InvalidAction,
                    action.actionId
                };
            }
            return delivery;
        }

        LikesProgram::Quic::QuicEngineOptions m_options; // 创建时固定的协议要求
        LikesProgram::Quic::QuicActionSink* m_actionSink = nullptr; // 非拥有动作端
        LikesProgram::Quic::QuicEventObserver* m_eventObserver = nullptr; // 非拥有事件端
        LikesProgram::Quic::QuicPacketProtectionProvider* m_packetProtectionProvider = nullptr;
        LikesProgram::Net::Address m_peer; // 最近一次 peer/path 地址
        LikesProgram::Quic::QuicState m_state = LikesProgram::Quic::QuicState::Handshaking; // 状态机
        LikesProgram::Quic::QuicTlsVersion m_tlsVersion = LikesProgram::Quic::QuicTlsVersion::Unknown; // 协商版本
        std::string m_negotiatedProtocol; // Engine 查询用的 ALPN 副本
        bool m_packetProtectionReady = false; // fake 外部 packet-protection 结果
        std::uint64_t m_nextActionId = 1; // fake action 标识
        LikesProgram::Quic::QuicPathState m_pathState = LikesProgram::Quic::QuicPathState::Unknown; // 路径状态
        std::size_t m_activeStreams = 0; // fake stream 计数
        std::uint64_t m_receivedDatagrams = 0; // 输入 datagram 计数
        std::uint64_t m_sentDatagrams = 0; // 输出 datagram 计数
        int m_lastError = 0; // 最近错误码
        bool m_started = false; // 是否已启动握手
    };
}

int main() {
    using namespace LikesProgram::Quic;

    RunQuicVarIntTests();
    RunQuicLongHeaderTests();
    RunQuicPacketNumberTests();
    RunQuicPacketNumberSpaceTests();
    RunQuicShortHeaderTests();
    RunQuicPacketHeaderProtectionTests();
    RunQuicRetransmissionQueueTests();
    RunQuicCongestionBudgetTests();
    RunQuicFrameTypeTests();
    RunQuicAckFrameTests();
    RunQuicAckApplicationTests();
    RunQuicAckDelayTests();
    RunQuicStreamFrameTests();
    RunQuicCryptoFrameTests();
    RunQuicStreamControlFrameTests();
    RunQuicConnectionCloseFrameTests();
    RunQuicConnectionIdFrameTests();
    RunQuicFlowControlFrameTests();
    RunQuicPathValidationFrameTests();
    RunQuicPingFrameTests();
    RunQuicDatagramFrameTests();
    RunQuicNewTokenFrameTests();
    RunQuicHandshakeDoneFrameTests();
    RunQuicTimeoutTests();
    RunQuicStreamCreditTests();
    RunQuicConnectionCreditTests();
    RunQuicCreditReservationTests();
    RunQuicAckTrackerTests();
    RunQuicAckRecoveryLedgerTests();
    RunQuicCreditCongestionLedgerTests();
    RunQuicPathMigrationLedgerTests();
    RunQuicConnectionIdTrackerTests();
    RunQuicPathProbeTrackerTests();
    RunQuicStreamTerminalTrackerTests();
    RunQuicWireEngineTests();

    QuicEngineOptions options; // 强制 TLS1.3 与 h3 ALPN
    Require(options.tlsVersion == QuicTlsVersion::Tls13);
    Require(std::string_view(options.applicationProtocol) == "h3");
    Require(IsValidQuicEngineOptions(options));

    auto invalidOptions = options;
    invalidOptions.tlsVersion = QuicTlsVersion::Unknown;
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.applicationProtocol = "webtransport";
    Require(IsValidQuicEngineOptions(invalidOptions));
    invalidOptions.applicationProtocol = "";
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.applicationProtocol = nullptr;
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.maximumDatagramBytes = 1199;
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.destinationConnectionIdLength = 21;
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.idleTimeout = std::chrono::milliseconds(-1);
    Require(!IsValidQuicEngineOptions(invalidOptions));
    invalidOptions = options;
    invalidOptions.role = static_cast<QuicRole>(0xFF);
    Require(!IsValidQuicEngineOptions(invalidOptions));

    ActionCollector actions; // 接收 fake UDP/timer/stream 控制动作
    EventCollector events; // 接收握手、stream 和 path 事件
    QuicTlsHandshakeResult progress;
    Require(ValidateQuicTlsHandshakeResult(options, progress)
        == QuicTlsHandshakeError::None);
    auto complete = progress;
    complete.state = QuicTlsHandshakeState::Complete;
    complete.tlsVersion = QuicTlsVersion::Tls13;
    complete.applicationProtocol = "h3";
    Require(ValidateQuicTlsHandshakeResult(options, complete)
        == QuicTlsHandshakeError::PacketProtectionUnavailable);
    complete.packetProtectionReady = true;
    Require(ValidateQuicTlsHandshakeResult(options, complete)
        == QuicTlsHandshakeError::None);
    auto wrongProtocol = complete;
    wrongProtocol.applicationProtocol = "h2";
    Require(ValidateQuicTlsHandshakeResult(options, wrongProtocol)
        == QuicTlsHandshakeError::ApplicationProtocolMismatch);
    auto failed = progress;
    failed.state = QuicTlsHandshakeState::Failed;
    failed.error = 77;
    Require(ValidateQuicTlsHandshakeResult(options, failed)
        == QuicTlsHandshakeError::None);

    QuicEngineFactory factory(
        [](const QuicEngineOptions& requested) {
            return std::make_unique<FakeQuicEngine>(requested);
        });
    QuicEngineFactory copied(factory); // 复制后仍共享回调状态
    Require(static_cast<bool>(factory));
    Require(static_cast<bool>(copied));
    Require(!factory.Create(invalidOptions));

    auto engine = copied.Create(options); // 通过公共 Factory 创建用户 backend
    Require(engine != nullptr);
    engine->SetActionSink(&actions);
    engine->SetEventObserver(&events);

    const std::array<std::uint8_t, 2> associatedData{ 0x01, 0x02 };
    const std::array<std::uint8_t, 3> plaintext{ 0x10, 0x11, 0x12 };
    std::array<std::uint8_t, 8> protectedBytes{};
    QuicPacketProtectionRequest protectionRequest{
        QuicPacketProtectionLevel::Handshake,
        7,
        associatedData,
        plaintext
    };
    Require(engine->ProtectPacket(protectionRequest, protectedBytes).error
        == QuicPacketProtectionError::NotReady);
    FakePacketProtection packetProtection;
    engine->SetPacketProtectionProvider(&packetProtection);
    const auto protectedResult = engine->ProtectPacket(protectionRequest, protectedBytes);
    Require(protectedResult.Succeeded());
    Require(protectedResult.bytesWritten == plaintext.size());
    Require(protectedBytes[1] == plaintext[1]);
    std::array<std::uint8_t, 8> plaintextOut{};
    const auto unprotectedResult = engine->UnprotectPacket(
        { QuicPacketProtectionLevel::Handshake, 7, associatedData, plaintext },
        plaintextOut);
    Require(unprotectedResult.Succeeded());
    Require(unprotectedResult.bytesWritten == plaintext.size());
    Require(plaintextOut[2] == plaintext[2]);
    std::array<std::uint8_t, 1> tooSmall{};
    Require(engine->ProtectPacket(protectionRequest, tooSmall).error
        == QuicPacketProtectionError::OutputTooSmall);
    const std::array<std::uint8_t, 1> authenticationFailure{ 0xFF };
    const auto failedProtection = engine->UnprotectPacket(
        { QuicPacketProtectionLevel::OneRtt, 8, associatedData, authenticationFailure },
        plaintextOut);
    Require(failedProtection.error == QuicPacketProtectionError::AuthenticationFailed);
    auto invalidProtectionRequest = protectionRequest;
    invalidProtectionRequest.level = static_cast<QuicPacketProtectionLevel>(0xFF);
    Require(ValidateQuicPacketProtectionRequest(invalidProtectionRequest, protectedBytes)
        == QuicPacketProtectionError::UnsupportedLevel);
    engine->SetPacketProtectionProvider(nullptr);
    Require(engine->UnprotectPacket(protectionRequest, plaintextOut).error
        == QuicPacketProtectionError::NotReady);

    const auto started = engine->StartHandshake();
    Require(started.Succeeded());
    Require(started.HasAction(QuicAction::DatagramReady));
    Require(started.HasAction(QuicAction::ArmTimer));
    Require(started.timerDelay == std::chrono::milliseconds(25));
    Require(actions.m_actions.size() == 1);
    Require(actions.m_actions.front().payload.AsStringView() == "CHLO");
    Require(actions.m_actions.front().actionId == 1);

LikesProgram::Net::Address peer("127.0.0.1", 443); // fake UDP peer，不建立真实 socket
    const auto handshake = engine->ConsumeDatagram(peer, MakeBuffer("SHLO"));
    Require(handshake.Succeeded());
    Require(handshake.HasAction(QuicAction::StreamEvent));
    Require(handshake.HasAction(QuicAction::PathEvent));
    Require(engine->State() == QuicState::Active);
    Require(engine->NegotiatedTlsVersion() == QuicTlsVersion::Tls13);
    Require(std::string_view(engine->NegotiatedProtocol()) == "h3");
    Require(engine->Snapshot().packetProtectionReady);
    Require(events.m_events.size() == 2);
    Require(events.m_events[0].kind == QuicEventKind::HandshakeComplete);
    Require(events.m_events[1].kind == QuicEventKind::PathValidated);
    Require(actions.m_actions.back().kind == QuicActionKind::CancelTimer);

    actions.m_nextDelivery = {
        QuicActionDeliveryState::Deferred,
        QuicActionDeliveryError::Backpressure
    };
    Require(engine->SendStreamData(8, MakeBuffer("deferred")).Succeeded());
    Require(actions.m_lastDelivery.Deferred());
    Require(actions.m_lastDelivery.Matches(actions.m_actions.back().actionId));

    auto mismatchEngine = factory.Create(options);
    Require(mismatchEngine != nullptr);
    mismatchEngine->SetActionSink(&actions);
    Require(mismatchEngine->StartHandshake().Succeeded());
    Require(mismatchEngine->ConsumeDatagram(peer, MakeBuffer("SHLO")).Succeeded());
    actions.m_nextDelivery = {
        QuicActionDeliveryState::Deferred,
        QuicActionDeliveryError::Backpressure,
        9999
    };
    Require(!mismatchEngine->SendStreamData(9, MakeBuffer("mismatch")).Succeeded());
    Require(actions.m_lastDelivery.Deferred());
    Require(!actions.m_lastDelivery.Matches(actions.m_actions.back().actionId));

    const std::array<std::uint8_t, 8> challengeToken{ 8, 7, 6, 5, 4, 3, 2, 1 };
    const auto responseActionResult = BuildQuicPathResponseAction({
        QuicPathValidationFrameKind::PathChallenge, challengeToken, 0 });
    Require(responseActionResult.IsOk());
    auto responseAction = responseActionResult.Value();
    responseAction.actionId = 2;
    actions.m_nextDelivery = {
        QuicActionDeliveryState::Deferred,
        QuicActionDeliveryError::Backpressure
    };
    const auto responseDelivery = actions.Submit(std::move(responseAction));
    Require(responseDelivery.Deferred());
    Require(actions.m_actions.back().payload.ReadableBytes() == 9);

    ActionCollector rejectingActions;
    rejectingActions.m_nextDelivery = {
        QuicActionDeliveryState::Rejected,
        QuicActionDeliveryError::TransportUnavailable
    };
    auto rejectedEngine = factory.Create(options);
    Require(rejectedEngine != nullptr);
    rejectedEngine->SetActionSink(&rejectingActions);
    Require(!rejectedEngine->StartHandshake().Succeeded());
    Require(rejectedEngine->State() == QuicState::Failed);

    auto progressEngine = factory.Create(options);
    Require(progressEngine != nullptr);
    progressEngine->SetActionSink(&actions);
    Require(progressEngine->StartHandshake().Succeeded());
    Require(progressEngine->ProvideTlsHandshakeResult(progress).Succeeded());
    Require(progressEngine->State() == QuicState::Handshaking);

    auto invalidHandshakeEngine = factory.Create(options);
    Require(invalidHandshakeEngine != nullptr);
    invalidHandshakeEngine->SetActionSink(&actions);
    Require(invalidHandshakeEngine->StartHandshake().Succeeded());
    const auto invalidHandshake = invalidHandshakeEngine->ProvideTlsHandshakeResult(wrongProtocol);
    Require(invalidHandshake.error
        == static_cast<int>(QuicTlsHandshakeError::ApplicationProtocolMismatch));
    Require(invalidHandshakeEngine->State() == QuicState::Failed);

    auto failedHandshakeEngine = factory.Create(options);
    Require(failedHandshakeEngine != nullptr);
    failedHandshakeEngine->SetActionSink(&actions);
    Require(failedHandshakeEngine->StartHandshake().Succeeded());
    const auto failedHandshake = failedHandshakeEngine->ProvideTlsHandshakeResult(failed);
    Require(failedHandshake.error == failed.error);
    Require(failedHandshakeEngine->State() == QuicState::Failed);

    const auto sent = engine->SendStreamData(4, MakeBuffer("request"));
    Require(sent.Succeeded());
    Require(sent.HasAction(QuicAction::DatagramReady));
    Require(actions.m_actions.back().streamId == 4);
    Require(actions.m_actions.back().payload.AsStringView() == "STREAMDATA");
    Require(engine->SendStreamFin(4).Succeeded());
    Require(engine->ResetStream(4, 7).Succeeded());
    Require(engine->StopSending(4, 8).Succeeded());
    Require(actions.m_actions[actions.m_actions.size() - 2].kind == QuicActionKind::ResetStream);
    Require(actions.m_actions.back().kind == QuicActionKind::StopSending);

    Require(engine->ConsumeDatagram(peer, MakeBuffer("STREAM")).Succeeded());
    Require(events.m_events.back().kind == QuicEventKind::StreamData);
    Require(events.m_events.back().payload.AsStringView() == "payload");
    Require(engine->ConsumeDatagram(peer, MakeBuffer("FIN")).Succeeded());
    Require(events.m_events.back().kind == QuicEventKind::StreamFin);
    Require(engine->ConsumeDatagram(peer, MakeBuffer("MIGRATE")).Succeeded());
    Require(engine->Snapshot().pathState == QuicPathState::Migrated);
    Require(events.m_events.back().kind == QuicEventKind::PathMigrated);

    Require(engine->Close(0x100).Succeeded());
    Require(engine->State() == QuicState::Closed);
    Require(actions.m_actions.back().kind == QuicActionKind::CloseConnection);

    std::atomic<int> initialized{ 0 }; // 复制 Factory 共享的一次性初始化次数
    QuicEngineFactory initializedFactory(
        [](const QuicEngineOptions& requested) {
            return std::make_unique<FakeQuicEngine>(requested);
        },
        [&initialized]() {
            initialized.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
    QuicEngineFactory initializedCopy(initializedFactory); // 共享初始化闸门
    Require(initializedFactory.InitializeSharedResources());
    Require(initializedCopy.InitializeSharedResources());
    Require(initialized.load(std::memory_order_relaxed) == 1);

    QuicEngineFactory throwingFactory(
        [](const QuicEngineOptions&) -> std::unique_ptr<QuicEngine> {
            throw std::runtime_error("fake create failure");
        });
    Require(!throwingFactory.Create(options));

    auto timeoutEngine = factory.Create(options); // 未启动成功的握手必须可超时失败
    Require(timeoutEngine != nullptr);
    Require(timeoutEngine->HandleTimeout().error != 0);
    Require(timeoutEngine->State() == QuicState::Failed);

    QuicEngineFactory empty; // 空 Factory 必须安全失败
    Require(!static_cast<bool>(empty));
    Require(empty.InitializeSharedResources());
    Require(!empty.Create(options));
}
