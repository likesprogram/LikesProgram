#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        enum class TlsVersion : std::uint8_t {
            Unknown,
            Tls12,
            Tls13
        };

        enum class TlsState : std::uint8_t {
            Handshaking,
            Active,
            Closing,
            Closed,
            Failed
        };

        enum class TlsAction : std::uint8_t {
            None = 0,
            NeedCiphertext = 1 << 0,
            CiphertextReady = 1 << 1,
            PlaintextReady = 1 << 2,
            CloseTransport = 1 << 3
        };

        // 合并同一次 Engine 推进产生的多个动作。
        constexpr TlsAction operator|(TlsAction left, TlsAction right) noexcept {
            return static_cast<TlsAction>(
                static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
        }

        // 判断结果是否包含指定动作。
        constexpr bool HasTlsAction(TlsAction actions, TlsAction expected) noexcept {
            return (static_cast<std::uint8_t>(actions) & static_cast<std::uint8_t>(expected))
                == static_cast<std::uint8_t>(expected);
        }

        struct TlsResult {
            TlsAction actions = TlsAction::None; // 本次推进后需要框架执行的动作集合
            int error = 0;                       // 0 表示成功，非 0 由 Engine 映射为稳定错误码

            // 返回本次 Engine 推进是否成功。
            bool Succeeded() const noexcept {
                return error == 0;
            }

            // 返回本次结果是否包含指定动作。
            bool HasAction(TlsAction expected) const noexcept {
                return HasTlsAction(actions, expected);
            }
        };

        class LIKESPROGRAM_NET_API TlsEngine {
        public:
            // 由实现所属模块释放每连接 TLS 会话资源。
            virtual ~TlsEngine();

            TlsEngine(const TlsEngine&) = delete;
            TlsEngine& operator=(const TlsEngine&) = delete;

            // 启动握手，并把需要发送的握手密文追加到 ciphertextOutput。
            virtual TlsResult StartHandshake(BufferChain& ciphertextOutput) = 0;
            // 消费 socket completion 提供的密文，并追加解密明文或握手响应密文。
            // 返回前必须消费、复制或由 Engine 内部保留全部输入，不能保存输入段借用指针。
            virtual TlsResult ConsumeCiphertext(
                BufferChain& ciphertextInput,
                BufferChain& plaintextOutput,
                BufferChain& ciphertextOutput) = 0;
            // 消费业务明文，并把加密结果追加到 ciphertextOutput。
            // 返回前必须消费或由 Engine 内部保留全部输入，避免调用结束后丢失待发送数据。
            virtual TlsResult ConsumePlaintext(
                BufferChain& plaintextInput,
                BufferChain& ciphertextOutput) = 0;
            // 启动 TLS 关闭，并把 close_notify 等密文追加到 ciphertextOutput。
            virtual TlsResult Shutdown(BufferChain& ciphertextOutput) = 0;
            // 返回当前每连接 TLS 状态。
            virtual TlsState State() const noexcept = 0;
            // 返回协商后的 ALPN 文本；未协商时返回空字符串，指针至少存活到下一次 Engine 调用。
            virtual const char* NegotiatedProtocol() const noexcept = 0;
            // 返回实际协商的 TLS 版本；旧 Engine 未提供时保持 Unknown。
            virtual TlsVersion NegotiatedVersion() const noexcept {
                return TlsVersion::Unknown;
            }

        protected:
            // 只允许派生 Engine 构造基类。
            TlsEngine() = default;
        };
    }
}
