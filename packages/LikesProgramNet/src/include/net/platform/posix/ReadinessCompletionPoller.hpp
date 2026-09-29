#pragma once

#include <LikesProgram/Net/Poller.hpp>
#include <memory>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ReadinessDriver;

            class ReadinessCompletionPoller final : public Poller {
            public:
                // 创建绑定 issuer 与私有 readiness driver 的 Poller。
                ReadinessCompletionPoller(
                    EventLoop* ownerLoop,
                    std::unique_ptr<ReadinessDriver> driver);
                // 释放 common Poller 状态与 driver。
                ~ReadinessCompletionPoller() override;

                ReadinessCompletionPoller(const ReadinessCompletionPoller&) = delete;
                ReadinessCompletionPoller& operator=(const ReadinessCompletionPoller&) = delete;

                // 启用私有 readiness driver。
                bool Activate() override;
                // 添加 Channel registration。
                bool AddChannel(Channel* channel) override;
                // 删除 Channel registration。
                bool RemoveChannel(Channel* channel) override;
                // 更新 Channel registration。
                bool UpdateChannel(Channel* channel) override;
                // 等待 readiness completion。
                void Poll(int timeoutMs, std::vector<Channel*>& active) override;
                // 提交当前积累的 operation。
                void Flush() override;
                // 提交 TCP connect completion。
                ConnectId StartConnect(
                    const Address& remoteAddress,
                    ConnectCallback callback) noexcept override;
                // 取消 TCP connect completion。
                void CancelConnect(ConnectId connectId) noexcept override;
                // 提交连接的直接 I/O。
                bool StartConnection(const std::shared_ptr<Connection>& connection) override;
                // 提交 listener accept completion。
                bool StartAccept(SocketType listenFd, AcceptCallback callback) override;
                // 取消 listener accept completion。
                void StopAccept(SocketType listenFd) override;
                // 提交可取消的 timer completion。
                TimeoutId ScheduleTimeout(
                    std::chrono::milliseconds delay,
                    TimeoutCallback callback) noexcept override;
                // 取消 timer completion。
                void CancelTimeout(TimeoutId timeoutId) noexcept override;
                // 排队连续 Buffer 写入。
                bool QueueWrite(Connection* connection, Buffer&& buffer) override;
                // 排队多段 BufferChain 写入。
                bool QueueWrite(Connection* connection, BufferChain&& chain) override;
                // 排队 UDP 数据报写入。
                bool QueueDatagramWrite(
                    Connection* connection,
                    const Address& peer,
                    Buffer&& buffer) override;
                // 更新连接读开关。
                void SetReadEnabled(Connection* connection, bool enabled) override;
                // 返回连接待写字节数。
                std::size_t PendingWriteBytes(const Connection* connection) const noexcept override;
                // 停止连接 operation。
                void StopConnection(Connection* connection) override;
                // 返回关闭路径待回收 completion。
                bool HasPendingShutdownCompletions() const noexcept override;
                // 返回已消费 completion 数。
                std::uint64_t CompletedOperationCount() const noexcept override;
                // 返回 provided-buffer 数量。
                std::size_t ProvidedBufferCount() const noexcept override;
                // 返回 read operation 提交数。
                std::uint64_t ReadSubmissionCount() const noexcept override;
                // 返回 accept operation 提交数。
                std::uint64_t AcceptSubmissionCount() const noexcept override;
                // 返回 receive bundle 能力。
                bool ReceiveBundleEnabled() const noexcept override;
                // 返回 completion 统计快照。
                CompletionStats GetCompletionStats() const noexcept override;
                // 返回 readiness driver 诊断名称。
                const char* BackendName() const noexcept override;

            private:
                // 撤销 readiness registration，迟到事件随后只按缺失 id 丢弃。
                void RemoveRegistration(std::uint64_t registrationId) noexcept;
                // 完成或取消一个 connect，并在 callback 前移除内部所有权。
                void CompleteConnect(ConnectId connectId, int error) noexcept;
                // drain listener 当前已排队连接并返回成功 completion 数。
                std::uint64_t DrainAccept(
                    SocketType listenFd,
                    std::uint64_t registrationId) noexcept;
                // 按当前读写状态更新 TCP Connection 的 readiness interest。
                bool UpdateConnectionInterest(Connection* connection) noexcept;
                // 有界 drain TCP 输入，并返回 logical completion 数。
                std::uint64_t DrainConnectionRead(
                    Connection* connection,
                    std::uint64_t registrationId,
                    bool drainForClose) noexcept;
                // 有界 drain TCP BufferChain，并返回 logical completion 数。
                std::uint64_t DrainConnectionWrite(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;
                // 有界 drain UDP 输入并保留 peer、原始长度与截断信息。
                std::uint64_t DrainDatagramRead(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;
                // 有界发送 UDP FIFO，并返回 logical completion 数。
                std::uint64_t DrainDatagramWrite(
                    Connection* connection,
                    std::uint64_t registrationId) noexcept;

                struct ReadinessCompletionPollerImpl;

                ReadinessCompletionPollerImpl* m_impl = nullptr; // common Poller PImpl
            };
        }
    }
}
