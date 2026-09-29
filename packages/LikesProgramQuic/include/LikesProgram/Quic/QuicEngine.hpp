#pragma once
#include <LikesProgram/Quic/system/LikesProgramQuicExport.hpp>
#include <LikesProgram/Quic/QuicFlowControlFrame.hpp>
#include <LikesProgram/Quic/QuicPacketProtection.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace LikesProgram {
    namespace Quic {
        using LikesProgram::Net::Address;
        using LikesProgram::Net::Buffer;

        enum class QuicRole : std::uint8_t {
            Client,
            Server
        };

        // QUIC 当前只允许 TLS 1.3，具体密码实现由 Engine 所属方提供。
        enum class QuicTlsVersion : std::uint8_t {
            Unknown,
            Tls13
        };

        enum class QuicState : std::uint8_t {
            Handshaking,
            Active,
            Closing,
            Closed,
            Failed
        };

        enum class QuicPathState : std::uint8_t {
            Unknown,
            Validating,
            Validated,
            Migrated
        };

        enum class QuicActionKind : std::uint8_t {
            DatagramReady,
            ArmTimer,
            CancelTimer,
            ResetStream,
            StopSending,
            CloseConnection,
            PathChallenge,
            PathResponse
        };

        enum class QuicEventKind : std::uint8_t {
            HandshakeComplete,
            CryptoData, // streamId carries the CRYPTO offset for this event
            StreamData,
            StreamFin,
            StreamReset,
            StopSending,
            ConnectionClose,
            PathValidated,
            PathMigrated
        };

        enum class QuicAction : std::uint16_t {
            None = 0,
            DatagramReady = 1 << 0,
            ArmTimer = 1 << 1,
            CancelTimer = 1 << 2,
            StreamEvent = 1 << 3,
            ConnectionClose = 1 << 4,
            PathEvent = 1 << 5
        };

        enum class QuicActionDeliveryState : std::uint8_t {
            Accepted,
            Deferred,
            Rejected
        };

        enum class QuicActionDeliveryError : int {
            None = 0,
            TransportUnavailable = 1,
            Backpressure = 2,
            InvalidAction = 3
        };

        struct QuicActionDelivery {
            QuicActionDeliveryState state = QuicActionDeliveryState::Accepted;
            QuicActionDeliveryError error = QuicActionDeliveryError::None;
            std::uint64_t actionId = 0;

            bool Matches(std::uint64_t expectedActionId) const noexcept {
                return expectedActionId != 0 && actionId == expectedActionId;
            }

            bool Accepted() const noexcept {
                return state == QuicActionDeliveryState::Accepted
                    && error == QuicActionDeliveryError::None;
            }

            bool Deferred() const noexcept {
                return state == QuicActionDeliveryState::Deferred;
            }

            bool Rejected() const noexcept {
                return state == QuicActionDeliveryState::Rejected;
            }
        };

        // 合并同一次 Engine 推进产生的多个动作。
        constexpr QuicAction operator|(QuicAction left, QuicAction right) noexcept {
            return static_cast<QuicAction>(
                static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
        }

        // 判断结果是否包含指定 QUIC 动作。
        constexpr bool HasQuicAction(QuicAction actions, QuicAction expected) noexcept {
            return (static_cast<std::uint16_t>(actions) & static_cast<std::uint16_t>(expected))
                == static_cast<std::uint16_t>(expected);
        }

        struct QuicEngineOptions {
            QuicRole role = QuicRole::Client; // Engine 固定的连接角色
            QuicTlsVersion tlsVersion = QuicTlsVersion::Tls13; // QUIC 强制的 TLS 版本
            const char* applicationProtocol = "h3"; // ALPN 文本，调用方保持其生命周期
            std::size_t maximumDatagramBytes = 1350; // UDP/QUIC 单包上限
            std::chrono::milliseconds idleTimeout{ 0 }; // 0 表示由 backend 自行决定
            QuicPacketHeaderProtectionProvider* packetHeaderProtectionProvider = nullptr;
            std::size_t destinationConnectionIdLength = 0;
            // Caller-owned destination CID bytes; the engine does not retain ownership.
            std::span<const std::uint8_t> destinationConnectionId{};
        };

        // 外部 TLS backend 在 QUIC handshake 边界上交接的进度。
        enum class QuicTlsHandshakeState : std::uint8_t {
            InProgress,
            Complete,
            Failed
        };

        // 结果不满足 Engine 选项时返回的稳定契约错误。
        enum class QuicTlsHandshakeError : int {
            None = 0,
            InvalidOptions = 1,
            TlsVersionMismatch = 2,
            ApplicationProtocolMismatch = 3,
            PacketProtectionUnavailable = 4,
            MissingFailureCode = 5,
            InvalidState = 6
        };

        struct QuicTlsHandshakeResult {
            QuicTlsHandshakeState state = QuicTlsHandshakeState::InProgress;
            QuicTlsVersion tlsVersion = QuicTlsVersion::Unknown;
            std::string_view applicationProtocol{}; // 调用期间借用，不由 Engine 保存
            bool packetProtectionReady = false; // 密钥材料仍由外部 TLS/QUIC backend 持有
            bool peerAuthenticated = false; // 认证策略由外部 TLS backend 决定
            int error = 0; // Failed 状态必须提供非零稳定错误码
        };

        // 校验外部 TLS/packet-protection 结果是否能满足 Engine 选项；不接触密钥或第三方类型。
        LIKESPROGRAM_QUIC_API QuicTlsHandshakeError ValidateQuicTlsHandshakeResult(
            const QuicEngineOptions& options,
            const QuicTlsHandshakeResult& result) noexcept;

        // 检查 QUIC/HTTP3 contract 的固定选项约束，不触碰任何 TLS/QUIC 实现。
        LIKESPROGRAM_QUIC_API bool IsValidQuicEngineOptions(
            const QuicEngineOptions& options) noexcept;

        struct QuicResult {
            QuicAction actions = QuicAction::None; // 本轮需要 Net 调度的动作集合
            int error = 0; // 0 表示成功，非 0 表示稳定错误码
            std::chrono::milliseconds timerDelay{ 0 }; // ArmTimer 使用的下一次 deadline

            // 返回本次 Engine 推进是否成功。
            bool Succeeded() const noexcept {
                return error == 0;
            }

            // 返回本次结果是否包含指定动作。
            bool HasAction(QuicAction expected) const noexcept {
                return HasQuicAction(actions, expected);
            }
        };

        struct QuicActionMessage {
            // 构造空 payload 的 action 消息。
            QuicActionMessage()
                : payload(0) {
            }

            // 构造带 stream/error 标识的控制 action。
            explicit QuicActionMessage(
                QuicActionKind actionKind,
                std::uint64_t id = 0,
                std::uint64_t code = 0)
                : kind(actionKind), streamId(id), errorCode(code), payload(0) {
            }

            QuicActionKind kind = QuicActionKind::DatagramReady; // Net 需要执行的动作
            std::uint64_t actionId = 0; // Engine 分配的单调 action 标识
            std::uint64_t streamId = 0; // 相关 bidirectional/unidirectional stream
            std::uint64_t errorCode = 0; // stream 或 connection error code
            std::chrono::milliseconds timerDelay{ 0 }; // timer action 的相对期限
            Address peer; // datagram/path action 的目标 peer
            Buffer payload; // datagram 或 control payload，由消息拥有
        };

        class LIKESPROGRAM_QUIC_API QuicActionSink {
        public:
            virtual ~QuicActionSink() = default;

            // 接收 Engine 产生的动作；调用期间消息所有权仍归调用者。
            virtual QuicActionDelivery Submit(QuicActionMessage&& action) noexcept = 0;
        };

        struct QuicStreamEvent {
            // 构造空 payload 的入站事件。
            QuicStreamEvent()
                : payload(0) {
            }

            QuicEventKind kind = QuicEventKind::HandshakeComplete; // 入站 stream/connection 事件
            std::uint64_t streamId = 0; // stream id; CryptoData uses this slot for CRYPTO offset
            std::uint64_t errorCode = 0; // peer reset/close 的错误码
            bool applicationError = false; // CONNECTION_CLOSE application-vs-transport kind
            Address peer; // 产生事件的当前 path peer
            Buffer payload; // 入站 stream data，事件拥有
        };

        class LIKESPROGRAM_QUIC_API QuicEventObserver {
        public:
            virtual ~QuicEventObserver() = default;

            // 观察 Engine 解出的 stream 或 path 事件，不拥有 Engine 状态。
            virtual void Observe(QuicStreamEvent&& event) noexcept = 0;
        };

        struct QuicEngineSnapshot {
            QuicState state = QuicState::Handshaking; // 当前连接状态
            QuicTlsVersion tlsVersion = QuicTlsVersion::Unknown; // 实际协商版本
            QuicPathState pathState = QuicPathState::Unknown; // 当前路径状态
            bool handshakeComplete = false; // 是否已完成 TLS/QUIC handshake
            bool packetProtectionReady = false; // 外部 packet-protection 是否已就绪
            std::size_t activeStreams = 0; // 当前活跃 stream 数
            std::size_t pendingDatagrams = 0; // 尚未提交给 sink 的 datagram 数
            std::uint64_t receivedDatagrams = 0; // 收到的 UDP datagram 数
            std::uint64_t sentDatagrams = 0; // 产生的 UDP datagram 数
            int lastError = 0; // 最近一次稳定错误码
            std::uint64_t observedAckFrames = 0; // Engine 已提交到账本的 ACK 数
            std::uint64_t largestAcknowledgedPacket = 0; // 最近观察到的最大 ACK 包号
            std::size_t pendingRetransmissions = 0; // 当前 recovery 账本中的待确认包数
            std::uint64_t peerConnectionCredit = 0; // peer 最近公布的 MAX_DATA
            std::uint64_t reservedConnectionCredit = 0; // 已预留但未释放的连接 credit
            std::size_t trackedStreamCredits = 0; // 已观察 MAX_STREAM_DATA 的 stream 数
            std::size_t congestionWindowBytes = 0; // Engine 会计使用的拥塞窗口
            std::size_t congestionInFlightBytes = 0; // Engine 会计中的在途字节
            bool pathChallengePending = false; // path ledger 是否等待匹配响应
            bool hasConnectionIdSequence = false; // 是否观察到 NEW_CONNECTION_ID
            std::uint64_t highestConnectionIdSequence = 0; // 最大已观察 CID 序号
            bool recoveryTimerArmed = false; // Engine 是否已向外部 scheduler 请求 recovery timer
            std::chrono::milliseconds recoveryTimerDelay{ 0 }; // 最近一次 recovery ArmTimer delay
            bool hasInitialPacketNumber = false; // Initial number-space observation
            std::uint64_t largestInitialPacketNumber = 0;
            bool hasHandshakePacketNumber = false; // Handshake number-space observation
            std::uint64_t largestHandshakePacketNumber = 0;
            bool hasZeroRttPacketNumber = false; // 0-RTT number-space observation
            std::uint64_t largestZeroRttPacketNumber = 0;
        };

        // UDP/QUIC/TLS1.3 的 socket-independent contract。
        // 该接口不包含第三方 TLS/QUIC 类型，具体 wire backend 由实现方提供。
        class LIKESPROGRAM_QUIC_API QuicEngine {
        public:
            // 由实现所属模块释放每连接 QUIC 状态。
            virtual ~QuicEngine();

            QuicEngine(const QuicEngine&) = delete;
            QuicEngine& operator=(const QuicEngine&) = delete;

            // 设置非拥有的动作输出端；传输框架负责把 datagram 交给 UDP。
            virtual void SetActionSink(QuicActionSink* sink) noexcept = 0;
            // 设置非拥有的入站 stream/path 事件观察端。
            virtual void SetEventObserver(QuicEventObserver* observer) noexcept = 0;
            // 设置用户拥有的 packet protection provider；nullptr 表示暂不可用。
            virtual void SetPacketProtectionProvider(
                QuicPacketProtectionProvider* provider) noexcept = 0;
            // 通过外部 provider 保护一个 QUIC packet payload。
            virtual QuicPacketProtectionResult ProtectPacket(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;
            // 通过外部 provider 解保护一个 QUIC packet payload。
            virtual QuicPacketProtectionResult UnprotectPacket(
                const QuicPacketProtectionRequest& request,
                std::span<std::uint8_t> output) noexcept = 0;
            // 启动握手并把初始 QUIC datagram/定时器动作提交给 sink。
            virtual QuicResult StartHandshake() = 0;
            // 接收外部 TLS/packet-protection backend 的握手进度；ALPN 字符串仅在调用期间借用。
            virtual QuicResult ProvideTlsHandshakeResult(
                const QuicTlsHandshakeResult& result) = 0;
            // Applies caller-decoded peer transport parameters such as the
            // initial MAX_DATA/MAX_STREAM_DATA limits. TLS parsing remains
            // caller-owned; engines without a wire credit ledger may return
            // FailedPrecondition.
            virtual Result<void> ApplyPeerFlowControl(
                const QuicFlowControlFrame& frame);
            // Starts validation of a caller-selected peer address. Token
            // generation, UDP routing, timers, and packet protection remain
            // caller-owned. Engines without migration support return an error.
            virtual QuicResult BeginPathValidation(
                const Address& candidate,
                const std::array<std::uint8_t, 8>& token);
            // 消费一个完整 UDP datagram；Engine 必须消费或接管输入所有权。
            virtual QuicResult ConsumeDatagram(const Address& peer, Buffer&& datagram) = 0;
            // 推进内部 loss/retransmit/idle timer。
            virtual QuicResult HandleTimeout() = 0;
            // 发送一段应用 stream data；调用后输入所有权归 Engine。
            virtual QuicResult SendStreamData(
                std::uint64_t streamId,
                Buffer&& plaintext) = 0;
            // 对指定 stream 发送 FIN。
            virtual QuicResult SendStreamFin(std::uint64_t streamId) = 0;
            // 对指定 stream 发送 RESET_STREAM。
            virtual QuicResult ResetStream(
                std::uint64_t streamId,
                std::uint64_t errorCode) = 0;
            // 对指定 stream 发送 STOP_SENDING。
            virtual QuicResult StopSending(
                std::uint64_t streamId,
                std::uint64_t errorCode) = 0;
            // 进入连接关闭状态并产生 CONNECTION_CLOSE 动作。
            virtual QuicResult Close(std::uint64_t errorCode = 0) = 0;
            // 返回当前连接状态。
            virtual QuicState State() const noexcept = 0;
            // 返回当前连接诊断快照。
            virtual QuicEngineSnapshot Snapshot() const noexcept = 0;
            // 返回协商后的 ALPN 文本；未完成握手时返回空字符串。
            virtual const char* NegotiatedProtocol() const noexcept = 0;
            // 返回实际协商的 TLS 版本；未完成握手时返回 Unknown。
            virtual QuicTlsVersion NegotiatedTlsVersion() const noexcept = 0;

        protected:
            // 只允许派生 Engine 构造基类。
            QuicEngine() = default;
        };
    }
}
