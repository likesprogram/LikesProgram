#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/DtlsDatagramBatch.hpp>
#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        enum class DtlsRole : std::uint8_t {
            Client,
            Server
        };

        enum class DtlsState : std::uint8_t {
            Handshaking,
            Active,
            Closing,
            Closed,
            Failed
        };

        enum class DtlsAction : std::uint16_t {
            None = 0,
            CiphertextReady = 1 << 0,
            PlaintextReady = 1 << 1,
            ArmRetransmitTimer = 1 << 2,
            CancelRetransmitTimer = 1 << 3,
            CloseSession = 1 << 4
        };

        // 合并同一次 Engine 推进产生的多个动作。
        constexpr DtlsAction operator|(DtlsAction left, DtlsAction right) noexcept {
            return static_cast<DtlsAction>(
                static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
        }

        // 判断动作集合是否包含指定 DTLS 动作。
        constexpr bool HasDtlsAction(DtlsAction actions, DtlsAction expected) noexcept {
            return (static_cast<std::uint16_t>(actions) & static_cast<std::uint16_t>(expected))
                == static_cast<std::uint16_t>(expected);
        }

        struct DtlsResult {
            DtlsAction actions = DtlsAction::None;       // 本次推进要求 Net 执行的动作集合
            int error = 0;                               // 0 表示成功，非 0 表示 fatal session error
            std::int64_t retransmitAfterMilliseconds = 0; // Engine 请求的下一次重传延迟

            // 返回本次 Engine 推进是否成功。
            bool Succeeded() const noexcept {
                return error == 0;
            }

            // 返回本次结果是否包含指定动作。
            bool HasAction(DtlsAction expected) const noexcept {
                return HasDtlsAction(actions, expected);
            }
        };

        struct DtlsSessionStats {
            std::uint64_t createdSessions = 0;    // 已创建的每 peer 会话总数
            std::size_t pendingSessions = 0;      // 当前 Handshaking 会话数量
            std::size_t activeSessions = 0;       // 当前 Active 会话数量
            std::uint64_t closedSessions = 0;     // 已完成清理的会话总数
            std::uint64_t handshakeTimeouts = 0;  // 握手总期限到期次数
            std::uint64_t retransmitTimeouts = 0; // Engine 重传 timer 到期次数
            std::uint64_t droppedNewPeers = 0;    // 资源上限拒绝的新 peer 数量
            std::uint64_t fatalSessionErrors = 0; // fatal session error 总数
        };

        class LIKESPROGRAM_NET_API DtlsEngine {
        public:
            // 由实现所属模块释放每 peer DTLS 会话资源。
            virtual ~DtlsEngine();

            DtlsEngine(const DtlsEngine&) = delete;
            DtlsEngine& operator=(const DtlsEngine&) = delete;

            // 启动 client 握手，并追加首个密文 flight。
            virtual DtlsResult StartHandshake(
                DtlsDatagramBatch& ciphertextOutput) = 0;
            // 消费一个完整密文数据报，并追加完整明文或响应密文数据报。
            virtual DtlsResult ConsumeCiphertext(
                Buffer& ciphertextDatagram,
                DtlsDatagramBatch& plaintextOutput,
                DtlsDatagramBatch& ciphertextOutput) = 0;
            // 消费一个完整业务明文数据报，并追加加密后的密文数据报。
            virtual DtlsResult ConsumePlaintext(
                Buffer& plaintextDatagram,
                DtlsDatagramBatch& ciphertextOutput) = 0;
            // 处理 Engine 请求的重传 timer 到期，并追加重传 flight。
            virtual DtlsResult HandleTimeout(
                DtlsDatagramBatch& ciphertextOutput) = 0;
            // 启动当前 peer 的协议关闭，并追加 alert/close 数据报。
            virtual DtlsResult Shutdown(
                DtlsDatagramBatch& ciphertextOutput) = 0;
            // 返回当前每 peer DTLS 会话状态。
            virtual DtlsState State() const noexcept = 0;
            // 返回协商后的协议文本；未协商时返回空字符串。
            virtual const char* NegotiatedProtocol() const noexcept = 0;

        protected:
            // 只允许派生 Engine 构造基类。
            DtlsEngine() = default;
        };
    }
}
