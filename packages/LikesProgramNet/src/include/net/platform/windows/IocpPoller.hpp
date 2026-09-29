#pragma once

#include <LikesProgram/Net/Poller.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct IocpPollerImpl;
            struct IocpChannelState;
            struct IocpOperation;
            struct IocpAcceptState;

            class IocpPoller final : public Poller {
            public:
                explicit IocpPoller(EventLoop* ownerLoop);
                ~IocpPoller() override;

                IocpPoller(const IocpPoller&) = delete;
                IocpPoller& operator=(const IocpPoller&) = delete;

                bool Activate() override;
                bool AddChannel(Channel* channel) override;
                bool RemoveChannel(Channel* channel) override;
                bool UpdateChannel(Channel* channel) override;
                void Poll(int timeoutMs, std::vector<Channel*>& active) override;
                void Flush() override;
                ConnectId StartConnect(
                    const Address& remoteAddress,
                    ConnectCallback callback) noexcept override;
                void CancelConnect(ConnectId connectId) noexcept override;
                bool StartConnection(const std::shared_ptr<Connection>& connection) override;
                bool StartAccept(SocketType listenFd, AcceptCallback callback) override;
                void StopAccept(SocketType listenFd) override;
                TimeoutId ScheduleTimeout(
                    std::chrono::milliseconds delay,
                    TimeoutCallback callback) noexcept override;
                void CancelTimeout(TimeoutId timeoutId) noexcept override;
                bool QueueWrite(Connection* connection, Buffer&& buffer) override;
                bool QueueWrite(Connection* connection, BufferChain&& chain) override;
                bool QueueDatagramWrite(
                    Connection* connection,
                    const Address& peer,
                    Buffer&& buffer) override;
                void SetReadEnabled(Connection* connection, bool enabled) override;
                std::size_t PendingWriteBytes(const Connection* connection) const noexcept override;
                void StopConnection(Connection* connection) override;
                bool HasPendingShutdownCompletions() const noexcept override;
                std::uint64_t CompletedOperationCount() const noexcept override;
                std::size_t ProvidedBufferCount() const noexcept override;
                std::uint64_t ReadSubmissionCount() const noexcept override;
                std::uint64_t AcceptSubmissionCount() const noexcept override;
                bool ReceiveBundleEnabled() const noexcept override;
                CompletionStats GetCompletionStats() const noexcept override;
                const char* BackendName() const noexcept override;

                // 仅供私有 contract 验证保留的控制 packet 入口。
                void PostTestPacket() noexcept;
                // 仅供私有 contract 验证读取当前 Channel generation。
                std::uint64_t ChannelRegistrationId(const Channel* channel) const noexcept;
                // 仅供私有 contract 验证投递迟到 Channel packet。
                void PostTestChannelPacket(std::uint64_t registrationId) noexcept;

            private:
                bool AssociateSocket(SocketType fd, ULONG_PTR completionKey) noexcept;
                bool EnsureConnectEx() noexcept;
                // 从当前 listener provider 加载 AcceptEx 入口。
                bool EnsureAcceptEx(SocketType listenFd) noexcept;
                // 为 listener 提交唯一 outstanding accept operation。
                bool SubmitAccept(IocpAcceptState& state) noexcept;
                void CompleteConnectOperation(
                    IocpOperation& operation,
                    bool syntheticCompletion) noexcept;
                // 消费 AcceptEx terminal packet 并转交 accepted socket。
                void CompleteAcceptOperation(
                    IocpOperation& operation,
                    bool syntheticCompletion) noexcept;
                bool ArmChannelState(IocpChannelState& state) noexcept;
                void RetireChannelState(IocpChannelState& state) noexcept;
                void ProcessChannelCompletion(
                    std::uint64_t registrationId,
                    std::vector<Channel*>& active,
                    std::uint64_t& batchItems) noexcept;

                IocpPollerImpl* m_impl = nullptr;
            };
        }
    }
}
