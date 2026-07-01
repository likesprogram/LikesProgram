#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/Transport.hpp>
#include <functional>
#include <memory>

namespace LikesProgram {
    namespace Net {
        class EventLoop;

        class LIKESPROGRAM_NET_API Connection {
        public:
            using Task = std::function<void()>;
            using CloseCallback = std::function<void(Connection&)>;

            enum class State {
                Connected,
                Closing,
                Closed
            };

            // 接管 socket 和传输层，transport 为空时默认创建 TcpTransport。
            Connection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport = nullptr);
            // 析构时强制关闭连接。
            virtual ~Connection();

            Connection(const Connection&) = delete;
            Connection& operator=(const Connection&) = delete;

            // 注册 Channel 并触发 OnConnected。
            void Start();
            // 返回底层 socket。
            SocketType GetSocket() const noexcept;
            // 返回当前连接状态。
            State GetState() const noexcept;
            // 返回连接是否仍处于 Connected 状态。
            bool IsConnected() const noexcept;
            // 设置框架内部关闭回调。
            void SetFrameworkCloseCallback(CloseCallback callback);

            // 发送 Buffer 的可读区域。
            void Send(const Buffer& buffer);
            // 发送一段字节。
            void Send(const void* data, std::size_t len);
            // 在 loop 线程中执行任务。
            void RunInLoop(Task task);
            // 将任务投递到 loop 线程。
            void QueueInLoop(Task task);
            // 优雅关闭，待发送缓冲清空后关闭写端。
            void Shutdown();
            // 强制关闭并丢弃未发送数据。
            void ForceClose();
            // 请求传输层升级通信，例如 STARTTLS 或自定义安全层协商。
            void UpgradeCommunication();

            // Reactor 读事件入口。
            void HandleRead();
            // Reactor 写事件入口。
            void HandleWrite();
            // Reactor 关闭事件入口。
            void HandleClose();
            // Reactor 错误事件入口。
            void HandleError();

            // 返回对端地址缓存。
            const Address& GetRemoteAddress() const noexcept;
            // 返回本端地址缓存。
            const Address& GetLocalAddress() const noexcept;

        protected:
            // 连接建立后调用。
            virtual void OnConnected() {}
            // 安全层会话初始化完成后调用；默认不进入握手，用户可在此调用 UpgradeCommunication。
            virtual void OnSecureLayerReady() {}
            // 安全层派生传输握手完成后调用；普通 TCP/UDP 不触发。
            virtual void OnHandshakeDone() {}
            // 收到数据后调用，业务应消费已处理字节。
            virtual void OnMessage(Buffer& in) { (void)in; }
            // 发送缓冲清空后调用。
            virtual void OnWriteComplete() {}
            // 关闭前调用。
            virtual void OnClosing() {}
            // 关闭完成后调用。
            virtual void OnClosed() {}
            // I/O 错误时调用。
            virtual void OnError(int error) { (void)error; }

        private:
            struct ConnectionImpl;

            // 在 loop 线程中追加待发送数据。
            void SendInLoop(const std::uint8_t* data, std::size_t len);
            // 推进安全层派生传输握手，普通 TCP/UDP 直接返回 true。
            bool AdvanceHandshake();
            // 根据传输层握手需求刷新 Channel 关注事件。
            void RefreshTransportInterest();
            // 根据发送缓冲状态启用写事件。
            void EnableWritingIfNeeded();
            // 执行统一关闭流程。
            void DoClose(bool notifyFramework);
            ConnectionImpl* m_impl = nullptr;                  // 连接实现，隐藏 transport/buffer/callback 状态
        };
    }
}
