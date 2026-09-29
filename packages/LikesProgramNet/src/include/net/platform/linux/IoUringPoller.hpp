#pragma once

#include <LikesProgram/Net/Poller.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class IoUringPoller final : public Poller {
            public:
                // 创建 Linux io_uring completion Poller。
                explicit IoUringPoller(EventLoop* ownerLoop);
                // 释放 ring、provided-buffer 与未完成操作。
                ~IoUringPoller() override;

                IoUringPoller(const IoUringPoller&) = delete;
                IoUringPoller& operator=(const IoUringPoller&) = delete;

                // 返回构造期 ring 与必要 buffer generation 是否完整。
                bool IsReady() const noexcept;
                // 在 EventLoop 线程启用 single-issuer ring。
                bool Activate() override;
                // 提交 Channel 对应的 poll completion。
                bool AddChannel(Channel* channel) override;
                // 取消 Channel 对应的 poll completion。
                bool RemoveChannel(Channel* channel) override;
                // 按最新关注集合重建 Channel poll completion。
                bool UpdateChannel(Channel* channel) override;
                // 等待并批量分派 CQE。
                void Poll(int timeoutMs, std::vector<Channel*>& active) override;
                // 批量提交当前 SQ 中积累的操作。
                void Flush() override;
                // 提交一次 io_uring TCP connect completion。
                ConnectId StartConnect(
                    const Address& remoteAddress,
                    ConnectCallback callback) noexcept override;
                // 取消尚未完成的 connect operation。
                void CancelConnect(ConnectId connectId) noexcept override;
                // 为 built-in TCP/UDP 提交直接 completion I/O。
                bool StartConnection(const std::shared_ptr<Connection>& connection) override;
                // 为 listener 提交 multishot accept。
                bool StartAccept(SocketType listenFd, AcceptCallback callback) override;
                // 取消 listener multishot accept。
                void StopAccept(SocketType listenFd) override;
                // 提交一次 io_uring timeout completion。
                TimeoutId ScheduleTimeout(
                    std::chrono::milliseconds delay,
                    TimeoutCallback callback) noexcept override;
                // 取消尚未完成的 timeout operation。
                void CancelTimeout(TimeoutId timeoutId) noexcept override;
                // 接管并排队一个异步写 Buffer。
                bool QueueWrite(Connection* connection, Buffer&& buffer) override;
                // 接管并排队多段异步写 BufferChain。
                bool QueueWrite(Connection* connection, BufferChain&& chain) override;
                // 接管并排队一个 connected 或显式 peer UDP 数据报。
                bool QueueDatagramWrite(
                    Connection* connection,
                    const Address& peer,
                    Buffer&& buffer) override;
                // 暂停或恢复 multishot recv。
                void SetReadEnabled(Connection* connection, bool enabled) override;
                // 返回连接仍待完成的异步写字节数。
                std::size_t PendingWriteBytes(const Connection* connection) const noexcept override;
                // 取消连接尚未完成的 recv/send。
                void StopConnection(Connection* connection) override;
                // 返回仍等待 terminal cancel CQE 的关闭状态。
                bool HasPendingShutdownCompletions() const noexcept override;
                // 返回已消费 CQE 总数。
                std::uint64_t CompletedOperationCount() const noexcept override;
                // 返回 buffer ring 已注册的 buffer 数量。
                std::size_t ProvidedBufferCount() const noexcept override;
                // 返回 multishot recv 提交次数。
                std::uint64_t ReadSubmissionCount() const noexcept override;
                // 返回 multishot accept 提交次数。
                std::uint64_t AcceptSubmissionCount() const noexcept override;
                // 返回 receive bundle 运行时启用状态。
                bool ReceiveBundleEnabled() const noexcept override;
                // 返回 CQ 与 provided-buffer 反馈快照。
                CompletionStats GetCompletionStats() const noexcept override;
                // 在 issuer 线程注册新 buffer group 并异步退役旧 generation。
                bool ReconfigureProvidedBuffers(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount) noexcept;
                // 返回稳定后端诊断名。
                const char* BackendName() const noexcept override;

            private:
                struct IoUringPollerImpl;

                IoUringPollerImpl* m_impl = nullptr; // Linux completion 后端状态
            };
        }
    }
}
