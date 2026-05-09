#pragma once
#include "Buffer.hpp"
#include "transports/Transport.hpp"
#include "transports/TlsTransport.hpp"
#include "Broadcast.hpp"
#include "Address.hpp"
#include <memory>
#include <functional>
#include <atomic>
#include <openssl/ssl.h>

namespace LikesProgram {
    namespace Net {
        class EventLoop; // 前向声明
        class Channel;   // 前向声明
        class Server;
        class Connection : public std::enable_shared_from_this<Connection> {
        public:
            using Task = std::function<void()>;
            using CloseCallback = std::function<void(Connection&)>;

            enum class State {
                Connected,
                Closing,   // 本端请求优雅关闭：等 outBuffer flush 后 shutdownWrite
                Closed
            };

            Connection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport);

            virtual ~Connection();

            void Start();

            // 失败回滚
            void FailedRollback();

            SocketType GetSocket() const noexcept;

            void SetChannel(Channel* ch) noexcept;

            // 发送数据
            void Send(const Buffer& buf);

            // 发送数据
            void Send(const void* data, size_t len);

            // 在事件循环线程执行任务
            void RunInLoop(Task task);

            // 将任务投递到事件循环线程
            void QueueInLoop(Task task);

            void AdoptChannel(std::unique_ptr<Channel> ch);

            // 关闭回调
            void SetFrameworkCloseCallback(CloseCallback cb);

            // 业务调用：优雅关闭（写完再 shutdownWrite）
            void Shutdown();

            // 业务调用：强制关闭（丢弃 outBuffer）
            void ForceClose();

            // Reactor 事件入口
            // 读事件
            void HandleRead();
            // 超时事件
            void HandleTimeout();
            // 写事件
            void HandleWrite();
            // 关闭事件
            void HandleClose();
            // 错误事件
            void HandleError();

            // 在当前明文响应写完后，将连接从 TcpTransport 升级为 TlsTransport。
            // 主要用于 SMTP STARTTLS：先发送 220，再复用当前 socket 进入 TLS 握手。
            // sslCtx 由外部管理，Connection 不负责释放。
            void StartTlsAfterWriteComplete(SSL_CTX* sslCtx, TlsMode mode = TlsMode::Server);

            // 当前连接是否已经切换为 TLS 传输。
            // 注意：返回 true 仅表示 Transport 已切换，不代表 TLS 握手一定完成。
            bool IsTlsEnabled() const noexcept { return m_tlsEnabled; }
        protected:
            // 连接建立完成（可用于发欢迎包、初始化状态）
            virtual void OnConnected() {}

            // 握手完成
            virtual void OnHandshakeDone() {}

            // 操作超时
            virtual void OnTimeout() {}

            // 收到数据：业务在这里进行粘包拆包、解析协议，并 Consume 已处理字节
            virtual void OnMessage(Buffer& in) { (void)in; }

            // 发送缓冲区清空
            virtual void OnWriteComplete() {}

            // 关闭前（可选：记录日志、统计）
            virtual void OnClosing() {}

            // 已关闭（业务清理资源）
            virtual void OnClosed() {}

            // 错误
            virtual void OnError(int err) { (void)err; }

            // 获取广播器
            std::shared_ptr<Broadcast> GetBroadcast() const noexcept;

            // 获取对端地址
            const Address& GetRemoteAddress() const noexcept;
            // 获取本段地址
            const Address& GetLocalAddress() const noexcept;
        private:
            void SetCloseCallbackInternal(CloseCallback cb);
            friend class Server;

            void SendInLoop(const uint8_t* data, size_t len);

            bool AdvanceHandshake();

            void DoClose(bool notifyServer);

            // Channel 控制
            void EnableReading();
            void DisableReading();

            void EnableWriting();
            void DisableWriting();

            void EnableWritingIfNeeded();

            void StartTlsAfterWriteCompleteInLoop(SSL_CTX* sslCtx, TlsMode mode);
            void UpgradeToTlsInLoop(SSL_CTX* sslCtx, TlsMode mode);
            void NotifyWriteComplete();
            void RefreshChannelEvents();
        private:
            SocketType m_fd = kInvalidSocket;
            EventLoop* m_loop = nullptr;
            Channel* m_channel = nullptr;
            std::unique_ptr<Channel> m_channelOwned; // 防泄漏/防悬空

            friend class MainEventLoop;
            std::shared_ptr<Broadcast> m_broadcast; // 广播器

            std::unique_ptr<Transport> m_transport;

            bool m_tlsEnabled = false;
            bool m_tlsUpgradePending = false;
            SSL_CTX* m_pendingSslCtx = nullptr;
            TlsMode m_pendingTlsMode = TlsMode::Server;

            Address m_remoteAddr; // 对端地址
            Address m_localAddr; // 本端地址

            Buffer m_inBuffer;
            Buffer m_outBuffer;

            CloseCallback m_onCloseInternal; // 仅框架用

            bool isFailedRollback = false;

            State m_state = State::Connected;
        };
    }
}
