#pragma once

#include <LikesProgram/Net/Poller.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct EpollDriverImpl;

            class EpollDriver final : public Poller {
            public:
                // 创建 epoll completion 后端但延迟绑定 issuer 线程资源。
                explicit EpollDriver(EventLoop* ownerLoop);
                // 撤销 registration、timer 与 epoll fd。
                ~EpollDriver() override;

                EpollDriver(const EpollDriver&) = delete;
                EpollDriver& operator=(const EpollDriver&) = delete;

                // 在 EventLoop issuer 线程创建 epoll 实例。
                bool Activate() override;
                // 使用稳定 registration id 添加 Channel。
                bool AddChannel(Channel* channel) override;
                // 先撤销 registration id，再删除 Channel。
                bool RemoveChannel(Channel* channel) override;
                // 原地更新 Channel 的 level-triggered 关注集合。
                bool UpdateChannel(Channel* channel) override;
                // 在调用方预算与最近 timer deadline 内等待 completion。
                void Poll(int timeoutMs, std::vector<Channel*>& active) override;
                // 提交延迟 connect 与 TCP/UDP 写队列。
                void Flush() override;
                // Task 6 前不提交 connect operation。
                ConnectId StartConnect(
                    const Address& remoteAddress,
                    ConnectCallback callback) noexcept override;
                // Task 6 前没有 connect operation 可取消。
                void CancelConnect(ConnectId connectId) noexcept override;
                // 启动 built-in TCP/UDP completion 输入。
                bool StartConnection(const std::shared_ptr<Connection>& connection) override;
                // Task 6 前不启动 accept operation。
                bool StartAccept(SocketType listenFd, AcceptCallback callback) override;
                // Task 6 前没有 accept operation 可停止。
                void StopAccept(SocketType listenFd) override;
                // 把一次性 callback 加入单调 deadline heap。
                TimeoutId ScheduleTimeout(
                    std::chrono::milliseconds delay,
                    TimeoutCallback callback) noexcept override;
                // 删除 active timer generation，旧 heap node 延迟丢弃。
                void CancelTimeout(TimeoutId timeoutId) noexcept override;
                // Task 7 前不接管单段 TCP 写所有权。
                bool QueueWrite(Connection* connection, Buffer&& buffer) override;
                // Task 7 前不接管多段 TCP/TLS 写所有权。
                bool QueueWrite(Connection* connection, BufferChain&& chain) override;
                // 接管一个完整 UDP/DTLS 数据报并保持 FIFO。
                bool QueueDatagramWrite(
                    Connection* connection,
                    const Address& peer,
                    Buffer&& buffer) override;
                // 切换 TCP/UDP 连接读取状态。
                void SetReadEnabled(Connection* connection, bool enabled) override;
                // 返回 TCP 字节或 UDP 队列成本的待发送总量。
                std::size_t PendingWriteBytes(const Connection* connection) const noexcept override;
                // 停止连接并同步回收全部 epoll-owned 队列。
                void StopConnection(Connection* connection) override;
                // 最小诊断后端没有 terminal completion。
                bool HasPendingShutdownCompletions() const noexcept override;
                // 最小诊断后端尚未消费 completion。
                std::uint64_t CompletedOperationCount() const noexcept override;
                // epoll 后端不使用 io_uring provided-buffer ring。
                std::size_t ProvidedBufferCount() const noexcept override;
                // Task 4 前没有 read operation 提交。
                std::uint64_t ReadSubmissionCount() const noexcept override;
                // Task 6 前没有 accept operation 提交。
                std::uint64_t AcceptSubmissionCount() const noexcept override;
                // epoll 后端不启用 io_uring receive bundle。
                bool ReceiveBundleEnabled() const noexcept override;
                // 返回尚无 completion 的零值诊断快照。
                CompletionStats GetCompletionStats() const noexcept override;
                // 返回稳定 epoll completion 诊断名。
                const char* BackendName() const noexcept override;

            private:
                // 撤销 operation registration，迟到事件随后只按缺失 id 丢弃。
                void RemoveOperationRegistration(std::uint64_t registrationId) noexcept;
                // 完成或失败一个 connect，并在 callback 前移除全部内部所有权。
                void CompleteConnect(ConnectId connectId, int error) noexcept;
                // drain listener 当前已排队连接并返回成功 completion 数。
                std::uint64_t DrainAccept(
                    SocketType listenFd,
                    std::uint64_t registrationId) noexcept;
                // 按当前 read/write 状态更新 Connection 的 level-triggered interest。
                bool UpdateConnectionInterest(Connection* connection) noexcept;
                // 有界 drain TCP 输入，并返回本轮 logical completion 数。
                std::uint64_t DrainConnectionRead(
                    Connection* connection,
                    std::uint64_t registrationId,
                    bool drainForClose) noexcept;
                // 有界 drain TCP BufferChain，并返回本轮 logical completion 数。
                std::uint64_t DrainConnectionWrite(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;
                // 有界 drain UDP 数据报并保留 peer、原始长度与截断元数据。
                std::uint64_t DrainDatagramRead(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;
                // 有界逐项发送 UDP FIFO，并返回本轮 logical completion 数。
                std::uint64_t DrainDatagramWrite(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;

                EpollDriverImpl* m_impl = nullptr; // 私有 epoll registration 与 timer 状态
            };
        }
    }
}
