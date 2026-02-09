#pragma once
#include "Buffer.hpp"
#include "Transport.hpp"
#include <memory>
#include <functional>
#include <atomic>

namespace LikesProgram {
    namespace Net {
        class EventLoop; // 前向声明
        class Channel;   // 前向声明
        class Server;
        class Connection : public std::enable_shared_from_this<Connection> {
        public:
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

            const SocketType GetSocket() const noexcept;

            void SetChannel(Channel* ch) noexcept;

            // 发送数据
            void Send(const Buffer& buf);

            // 发送数据
            void Send(const void* data, size_t len);

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

            const Server* GetServer() const;
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
        private:
            SocketType m_fd = kInvalidSocket;
            EventLoop* m_loop = nullptr;
            Channel* m_channel = nullptr;
            std::unique_ptr<Channel> m_channelOwned; // 防泄漏/防悬空

            std::unique_ptr<Transport> m_transport;

            Buffer m_inBuffer;
            Buffer m_outBuffer;

            CloseCallback m_onCloseInternal; // 仅框架用

            bool isFailedRollback = false;

            State m_state = State::Connected;
        };
    }
}
