#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Poller.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class DtlsSessionManager;
            class TcpConnector;
        }

        class Connection;
        class Buffer;
        class BufferChain;
        class Server;

        class LIKESPROGRAM_NET_API EventLoop {
        public:
            using Task = std::function<void()>;

            // 使用平台默认 Poller 创建事件循环。
            EventLoop();
            // 使用调用方提供的 Poller 创建事件循环。
            explicit EventLoop(std::unique_ptr<Poller> poller);
            virtual ~EventLoop();

            EventLoop(const EventLoop&) = delete;
            EventLoop& operator=(const EventLoop&) = delete;

            // 在当前线程启动事件循环，直到 Shutdown 被调用。
            void Start();
            // 请求事件循环停止，跨线程调用会在短轮询超时后生效。
            void Shutdown();
            // 返回事件循环是否已经成功启用 Poller 并进入主循环。
            bool IsRunning() const noexcept;
            // 返回当前线程是否为事件循环线程。
            bool IsInLoopThread() const noexcept;
            // 返回当前 completion I/O 后端名称，未启用时返回 none。
            const char* CompletionBackendName() const noexcept;
            // 返回当前 EventLoop 已完成的 completion I/O 操作数。
            std::uint64_t CompletionOperationCount() const noexcept;
            // 返回当前 Poller 注册的 provided-buffer 数量。
            std::size_t CompletionProvidedBufferCount() const noexcept;
            // 返回 receive bundle 是否已通过运行时能力探测并启用。
            bool CompletionReceiveBundleEnabled() const noexcept;
            // 返回 CQ 与 provided-buffer 反馈快照。
            CompletionStats GetCompletionStats() const noexcept;
            // 返回当前 Poller 提交的直接读操作数量。
            std::uint64_t CompletionReadSubmissionCount() const noexcept;
            // 注册一个 Channel。
            bool RegisterChannel(Channel* channel);
            // 注销一个 Channel。
            bool UnregisterChannel(Channel* channel);
            // 更新一个 Channel 的关注事件。
            bool UpdateChannel(Channel* channel);
            // 投递一个任务到事件循环线程顺序执行。
            void PostTask(Task task);
            // 由 Server/Client 持有连接，避免 Channel 回调悬空。
            void AttachConnection(const std::shared_ptr<Connection>& connection);
            // 移除指定 socket 对应的连接持有，并返回移除前快照。
            std::shared_ptr<Connection> DetachConnection(SocketType fd);
            // 设置轮询超时，单位毫秒。
            void SetPollTimeout(int timeoutMs) noexcept;

        protected:
            // 处理 Poller 返回的活跃 Channel。
            virtual void ProcessEvents(const std::vector<Channel*>& activeChannels);
            // 执行待处理任务队列。
            void ProcessPendingTasks();
            // 返回底层 Poller 引用，仅供子类在 loop 线程内使用。
            Poller& PollerRef();

        private:
            friend class Connection;
            friend class Server;
            friend class Internal::DtlsSessionManager;
            friend class Internal::TcpConnector;

            struct EventLoopImpl;

            // 为已由当前 loop 持有的连接启用 Poller 直接 I/O。
            bool StartPollerIo(Connection* connection);
            // 为 listener 提交 multishot accept completion。
            bool StartPollerAccept(SocketType listenFd, Poller::AcceptCallback callback);
            // 取消 listener 尚未完成的 accept completion。
            void StopPollerAccept(SocketType listenFd);
            // 在当前 Poller 提交一次定时 completion。
            Poller::TimeoutId SchedulePollerTimeout(
                std::chrono::milliseconds delay,
                Task task);
            // 取消尚未完成的定时 completion。
            void CancelPollerTimeout(Poller::TimeoutId timeoutId) noexcept;
            // 把一个连接写 Buffer 的所有权交给 Poller 排队。
            bool QueuePollerWrite(Connection* connection, Buffer&& buffer);
            // 把一个连接写 BufferChain 的所有权交给 Poller 排队。
            bool QueuePollerWrite(Connection* connection, BufferChain&& chain);
            // 把一个完整 UDP 数据报及显式 peer 交给 Poller 排队。
            bool QueuePollerDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer);
            // 更新 Poller 直接连接的业务读开关。
            void SetPollerReadEnabled(Connection* connection, bool enabled);
            // 返回 Poller 直接连接仍待写出的字节数。
            std::size_t PollerPendingWriteBytes(const Connection* connection) const noexcept;
            // 停止连接对应的 Poller 操作。
            void StopPollerIo(Connection* connection);
            EventLoopImpl* m_impl = nullptr;                          // 事件循环实现，隐藏线程与 STL 状态
        };
    }
}
