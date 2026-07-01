#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        enum class TransportKind {
            Tcp,
            Udp
        };

        // 判断传输类型是否基于 UDP 数据报。
        constexpr bool IsDatagramTransport(TransportKind kind) noexcept {
            return kind == TransportKind::Udp;
        }

        enum class IoStatus {
            Ok,
            WouldBlock,
            PeerClosed,
            Error
        };

        enum class TransportSecurityState {
            Plain,
            Handshaking,
            Secure
        };

        struct IoResult {
            IoStatus status = IoStatus::Error; // 本次 I/O 结果类型
            std::int64_t nbytes = 0;           // Ok 时表示实际读写字节数
            int error = 0;                     // Error 时保存系统错误码
        };

        class LIKESPROGRAM_NET_API Transport {
        public:
            // 接管一个已打开 socket，生命周期由 Transport 负责。
            explicit Transport(SocketType fd);
            // 子类析构负责关闭或分离具体传输资源。
            virtual ~Transport();

            Transport(const Transport&) = delete;
            Transport& operator=(const Transport&) = delete;

            // 返回底层 socket，调用方不得关闭。
            SocketType Fd() const noexcept;
            // 解除所有权并返回底层 socket，后续 Close 不再关闭它。
            SocketType DetachFd() noexcept;
            // 返回当前传输族，用于 Server/Client 与业务层诊断分流。
            virtual TransportKind Kind() const noexcept = 0;
            // 返回当前安全层状态，默认保持普通 socket 明文态。
            TransportSecurityState SecurityState() const noexcept;
            // 返回安全层是否已经接管后续读写。
            bool SecureCommunicationReady() const noexcept;

            // 从传输层读入缓冲，非阻塞场景下可能返回 WouldBlock。
            virtual IoResult ReadSome(Buffer& in) = 0;
            // 从给定内存写入传输层，非阻塞场景下可能返回 WouldBlock。
            virtual IoResult WriteSome(const std::uint8_t* data, std::size_t len) = 0;
            // 优雅关闭写端。
            virtual void ShutdownWrite() = 0;
            // 关闭传输层资源。
            virtual void Close() = 0;

            // 初始化本连接 TLS/SSL 会话资源，共享证书上下文应由外部注入。
            virtual IoResult InitializeSecureLayer();
            // 执行 STARTTLS 类通信升级，默认明文传输保持原状。
            virtual IoResult UpgradeCommunication();
            // 安全层派生 transport 可覆盖该入口表示握手仍需推进。
            virtual bool NeedHandshake() const;
            // 安全层派生 transport 可覆盖该入口执行一次握手推进。
            virtual IoResult Handshake();
            // 握手阶段是否需要继续关注读事件。
            virtual bool RemainWantRead() const;
            // 握手阶段是否需要继续关注写事件。
            virtual bool RemainWantWrite() const;

        protected:
            // 将连接切入安全层握手态，通常由派生类在 UpgradeCommunication 中调用。
            void BeginSecureHandshake() noexcept;
            // 标记安全层握手完成，后续 ReadSome/WriteSome 可切换到安全读写钩子。
            void CompleteSecureHandshake() noexcept;
            // 回到普通 socket 明文态，供派生类在协商失败后实现自定义降级。
            void ResetToPlainCommunication() noexcept;
            // 返回安全层是否已经开始接管连接生命周期。
            bool SecureLayerActive() const noexcept;
            // 返回当前 socket，供派生类执行平台 I/O。
            SocketType CurrentSocket() const noexcept;
            // 更新当前 socket，通常仅关闭或 detach 后使用。
            void SetCurrentSocket(SocketType fd) noexcept;
            // 标记 socket 已关闭，返回调用方是否为首个关闭者。
            bool MarkClosedOnce() noexcept;
            // 直接标记为已关闭，用于 detach 后取消所有权。
            void MarkClosed() noexcept;
            // 构造成功结果。
            static IoResult MakeOk(std::int64_t nbytes);
            // 构造非阻塞暂不可读写结果。
            static IoResult MakeWouldBlock();
            // 构造对端关闭结果。
            static IoResult MakePeerClosed();
            // 构造系统错误结果。
            static IoResult MakeError(int error);

        private:
            struct TransportImpl;

            TransportImpl* m_impl = nullptr;           // 传输实现，隐藏原子状态和 socket 所有权
        };
    }
}
