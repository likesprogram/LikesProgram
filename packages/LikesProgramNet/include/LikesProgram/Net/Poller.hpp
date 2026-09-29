#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class EventLoop;
        class Address;
        class Connection;
        class Buffer;
        class BufferChain;

        struct CompletionStats {
            std::uint64_t completedOperations = 0;       // 已消费 CQE 总数
            std::uint64_t connectSubmissions = 0;        // 已提交 TCP connect operation 数
            std::uint64_t connectCancelSubmissions = 0;  // 已提交 connect cancel operation 数
            std::size_t pendingConnectOperations = 0;    // 尚未回收原 CQE 的 connect 数
            std::uint64_t completionBatchCount = 0;      // 至少包含一个 CQE 的 Poll 批次数
            std::uint64_t completionBatchItems = 0;      // 全部批次 CQE 数之和，用于计算平均值
            std::uint64_t peakCompletionBatch = 0;       // 单次 Poll 消费 CQE 峰值
            std::uint64_t enobufsCompletions = 0;        // provided-buffer 饥饿 completion 次数
            std::uint64_t cqOverflowCount = 0;           // 内核 CQ overflow 累计计数
            std::uint64_t receivedBytes = 0;             // provided-buffer read completion 总字节数
            std::uint64_t receiveCompletions = 0;        // 成功 provided-buffer read completion 数
            std::uint64_t leaseHoldSampleCount = 0;      // lease hold time 采样数量
            std::uint64_t totalLeaseHoldNanoseconds = 0; // 采样 hold time 总纳秒数
            std::uint64_t maximumLeaseHoldNanoseconds = 0; // 采样 hold time 峰值纳秒数
            std::size_t providedBufferCount = 0;         // 注册 provided-buffer 总数
            std::size_t providedBufferSize = 0;          // 单个 provided-buffer 字节容量
            std::size_t activeBufferLeases = 0;          // 仍由业务 Buffer 持有的 lease 数
            std::size_t pendingBufferReturns = 0;        // 等待 issuer 回填 ring 的 token 数
            std::size_t currentAvailableBuffers = 0;     // 当前估算可由内核选择的 buffer 数
            std::size_t minimumAvailableBuffers = 0;     // 运行期最低可用 buffer 水位
            std::size_t completionBudget = 0;            // 当前单轮 CQE 公平预算
            std::size_t retiringProvidedBufferGenerations = 0; // 等待旧 read CQE 回收的 generation 数
            std::size_t retainedProvidedBufferGenerations = 0; // ring 已注销但仍有 lease 的 generation 数
            std::size_t retainedProvidedBufferBytes = 0; // 旧 lease 延长的 pool 字节总量
            std::uint64_t providedBufferReconfigurationCount = 0; // 已接受的安全 generation 切换次数
            std::uint64_t providedBufferPolicyEvaluationCount = 0; // 已处理的运行时反馈窗口数
            std::uint64_t providedBufferPolicyRecommendationCount = 0; // 产生 generation 候选的窗口数
            bool receiveBundleEnabled = false;           // receive bundle 运行时启用状态
            std::uint64_t receiveBundleCompletions = 0;  // 实际按 bundle 语义拆包的成功 CQE 数
            std::uint64_t receiveBundleBuffers = 0;      // bundle CQE 累计消费的 provided-buffer 数
            std::uint64_t maximumReceiveBundleBuffers = 0; // 单个 bundle CQE 消费 buffer 峰值
            std::uint64_t datagramReceiveCompletions = 0; // 成功 UDP recvmsg CQE 数
            std::uint64_t datagramSendCompletions = 0;    // 成功 UDP sendmsg CQE 数
            std::uint64_t receivedDatagrams = 0;          // 已交付业务的 UDP 数据报数
            std::uint64_t sentDatagrams = 0;              // 已完整发送的 UDP 数据报数
            std::uint64_t zeroLengthDatagrams = 0;        // 收发零长度 UDP 数据报总数
            std::uint64_t truncatedDatagrams = 0;         // 接收容量不足的数据报数
            std::size_t pendingDatagramSends = 0;         // 当前等待 CQE 的 UDP 数据报数
            bool datagramMultishotEnabled = false;        // UDP multishot 与独立 buffer group 可用状态
            std::size_t datagramProvidedBufferCount = 0;  // UDP 专用 provided-buffer 数量
            std::size_t datagramProvidedBufferSize = 0;   // UDP 专用 provided-buffer 字节容量
            std::uint64_t datagramMultishotReceiveCompletions = 0; // multishot UDP 成功 CQE 数
            std::uint64_t datagramReceiveFallbacks = 0;   // 能力错误触发 one-shot 回退次数
            std::uint64_t datagramSendBatchSubmissions = 0; // 至少含一个数据报的发送批次数
            std::size_t maximumDatagramSendBatch = 0;     // 单批 UDP 发送数据报峰值
            std::size_t datagramActiveBufferLeases = 0;   // 当前仍由业务 Buffer 持有的 UDP token 数
            std::size_t datagramPendingBufferReturns = 0; // 已释放但尚未由 issuer 回填的 UDP token 数
            std::size_t datagramCurrentAvailableBuffers = 0; // 当前可供内核选择的 UDP token 数
        };

        class LIKESPROGRAM_NET_API Poller {
        public:
            using FdKey = std::uintptr_t;
            using AcceptCallback = std::function<void(SocketType)>;
            using ConnectId = std::uint64_t;
            using ConnectCallback = std::function<void(SocketType, int)>;
            using TimeoutId = std::uint64_t;
            using TimeoutCallback = std::function<void()>;

            static constexpr ConnectId InvalidConnectId = 0;
            static constexpr TimeoutId InvalidTimeoutId = 0;

            // 创建未绑定 EventLoop 的轮询器。
            Poller();
            // 创建绑定指定 EventLoop 的轮询器。
            explicit Poller(EventLoop* ownerLoop);
            virtual ~Poller();

            Poller(const Poller&) = delete;
            Poller& operator=(const Poller&) = delete;

            // 设置所属 EventLoop。
            void SetEventLoop(EventLoop* ownerLoop) noexcept;
            // 在 EventLoop 线程启用当前平台 completion 后端。
            virtual bool Activate() = 0;
            // 添加 Channel，由平台后端把事件转换为 completion。
            virtual bool AddChannel(Channel* channel) = 0;
            // 删除 Channel，并取消尚未完成的平台操作。
            virtual bool RemoveChannel(Channel* channel) = 0;
            // 更新 Channel 关注事件，并重建对应的平台操作。
            virtual bool UpdateChannel(Channel* channel) = 0;
            // 等待 completion，并填充需要执行回调的 Channel。
            virtual void Poll(int timeoutMs, std::vector<Channel*>& active) = 0;
            // 批量提交当前积累的平台操作。
            virtual void Flush() = 0;
            // 提交一次 TCP connect completion；成功回调接管 socket，失败回调收到错误码。
            virtual ConnectId StartConnect(
                const Address& remoteAddress,
                ConnectCallback callback) noexcept = 0;
            // 取消尚未完成的 connect completion，并在 CQE 回收后关闭 socket。
            virtual void CancelConnect(ConnectId connectId) noexcept = 0;
            // 为 built-in TCP/UDP 连接提交直接异步 I/O。
            virtual bool StartConnection(const std::shared_ptr<Connection>& connection) = 0;
            // 为 listener 提交长期 multishot accept。
            virtual bool StartAccept(SocketType listenFd, AcceptCallback callback) = 0;
            // 取消 listener 尚未完成的 multishot accept。
            virtual void StopAccept(SocketType listenFd) = 0;
            // 提交一次可取消的定时 completion；失败返回 InvalidTimeoutId。
            virtual TimeoutId ScheduleTimeout(
                std::chrono::milliseconds delay,
                TimeoutCallback callback) noexcept = 0;
            // 取消尚未完成的定时 completion。
            virtual void CancelTimeout(TimeoutId timeoutId) noexcept = 0;
            // 接管 Buffer 并排队异步写，CQE 完成前由 Poller 保持所有权。
            virtual bool QueueWrite(Connection* connection, Buffer&& buffer) = 0;
            // 接管多段 BufferChain 并排队异步写，不合并复制 Engine 输出。
            virtual bool QueueWrite(Connection* connection, BufferChain&& chain) = 0;
            // 接管一个完整 UDP 数据报并按 connected 或显式 peer 语义排队发送。
            virtual bool QueueDatagramWrite(
                Connection* connection,
                const Address& peer,
                Buffer&& buffer) = 0;
            // 更新直接异步连接的业务读开关。
            virtual void SetReadEnabled(Connection* connection, bool enabled) = 0;
            // 返回直接异步连接仍待完成的写字节数。
            virtual std::size_t PendingWriteBytes(const Connection* connection) const noexcept = 0;
            // 停止连接并取消尚未完成的平台操作。
            virtual void StopConnection(Connection* connection) = 0;
            // 返回关闭路径仍需 EventLoop 轮询回收的 terminal completion。
            virtual bool HasPendingShutdownCompletions() const noexcept { return false; }
            // 返回已消费 completion 数，供运行时诊断与回归验证。
            virtual std::uint64_t CompletedOperationCount() const noexcept = 0;
            // 返回 provided-buffer 数量，不支持的平台返回 0。
            virtual std::size_t ProvidedBufferCount() const noexcept = 0;
            // 返回直接读操作提交次数，供 multishot 回归验证。
            virtual std::uint64_t ReadSubmissionCount() const noexcept = 0;
            // 返回 multishot accept 提交次数。
            virtual std::uint64_t AcceptSubmissionCount() const noexcept = 0;
            // 返回 receive bundle 是否已通过运行时能力探测并启用。
            virtual bool ReceiveBundleEnabled() const noexcept = 0;
            // 返回 CQ 与 provided-buffer 反馈快照。
            virtual CompletionStats GetCompletionStats() const noexcept = 0;
            // 返回当前 completion 后端的稳定诊断名。
            virtual const char* BackendName() const noexcept = 0;
            // 查询轮询器是否持有指定 Channel。
            bool HasChannel(const Channel* channel) const;
            // 返回最近一次系统错误码。
            int LastError() const noexcept;

        protected:
            // 将 socket 转为 unordered_map 稳定 key。
            static FdKey ToKey(SocketType fd) noexcept;
            // 保存最近一次系统错误码。
            void SetLastError(int error) noexcept;
            // 保存 Channel 到内部 fd 映射。
            bool StoreChannel(Channel* channel);
            // 从内部 fd 映射删除 Channel。
            bool EraseChannel(Channel* channel);
            // 查找内部 fd 映射中的 Channel。
            Channel* FindStoredChannel(SocketType fd) const;
            // 返回内部 fd 映射是否为空。
            bool StoredChannelsEmpty() const noexcept;
            // 返回内部 fd 映射快照，避免派生类暴露 unordered_map。
            std::vector<Channel*> StoredChannelsSnapshot() const;

        private:
            struct PollerImpl;

            PollerImpl* m_impl = nullptr;                         // 轮询器实现，避免导出类携带 STL 容器
        };

        // 创建当前平台默认 completion Poller。
        LIKESPROGRAM_NET_API std::unique_ptr<Poller> CreateDefaultPoller(EventLoop* ownerLoop);
    }
}
