#include <LikesProgram/Net/Net.hpp>
#include "net/Transport.hpp"
#include "net/TcpTransport.hpp"
#include "net/UdpTransport.hpp"
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include <LikesProgram/Net/Protocol.hpp>
#include <LikesProgram/Core/Version.hpp>
#include "net/BufferLeaseAccess.hpp"
#include "net/ConnectionIdentityPolicy.hpp"
#include "net/OperationCancellationPolicy.hpp"
#include "net/PollerAccess.hpp"
#include "net/platform/linux/IoUringPoller.hpp"
#include "net/platform/linux/ProvidedBufferPolicy.hpp"
#include "net/platform/linux/ProvidedBufferPool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace {
    static_assert(
        LikesProgram::Net::IsDatagramTransport(LikesProgram::Net::TransportKind::Udp),
        "Protocol.hpp should expose UDP datagram classification without public Transport hierarchy");
    static_assert(
        offsetof(LikesProgram::Net::CompletionStats, datagramActiveBufferLeases)
            > offsetof(LikesProgram::Net::CompletionStats, maximumDatagramSendBatch),
        "UDP pool diagnostics must remain append-only in CompletionStats");

    // Net 回归测试覆盖基础缓冲、地址解析、本地 TCP/UDP 往返和安全传输扩展接口。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool IsEpollBackend(const char* name) noexcept {
        return name != nullptr && std::strcmp(name, "epoll-level-completion") == 0;
    }

    bool IsIoUringBackend(const char* name) noexcept {
        return name != nullptr
            && std::strcmp(name, "io_uring-multishot-provided-buffer") == 0;
    }

    bool IsKnownLinuxBackend(const char* name) noexcept {
        return IsEpollBackend(name) || IsIoUringBackend(name);
    }

#if defined(__linux__)
    void IgnorePollInterruptSignal(int) noexcept {
    }
#endif

    bool SetTestNonBlocking(LikesProgram::Net::SocketType fd) noexcept {
#ifdef _WIN32
        u_long mode = 1UL; // 测试用非阻塞模式，避免 UDP 接收异常时卡住。
        return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
        const int flags = ::fcntl(fd, F_GETFL, 0); // 保留现有 fd 标志后追加 O_NONBLOCK。
        return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    class OwnershipPoller final : public LikesProgram::Net::Poller {
    public:
        // 测试 Poller 只观察移动写入所有权，不执行真实平台 I/O。
        explicit OwnershipPoller(bool activationSucceeds = true)
            : Poller(nullptr),
            m_write(0),
            m_activationSucceeds(activationSucceeds) {
        }

        bool Activate() override { return m_activationSucceeds; }
        bool AddChannel(LikesProgram::Net::Channel*) override {
            m_channelAddCount.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        bool RemoveChannel(LikesProgram::Net::Channel*) override { return true; }
        bool UpdateChannel(LikesProgram::Net::Channel*) override { return true; }
        void Poll(int, std::vector<LikesProgram::Net::Channel*>& active) override {
            active.clear();
            m_pollCount.fetch_add(1, std::memory_order_acq_rel);
            if (m_shutdownPollsRemaining > 0) --m_shutdownPollsRemaining;
        }
        void Flush() override {}
        ConnectId StartConnect(
            const LikesProgram::Net::Address&,
            ConnectCallback) noexcept override {
            return InvalidConnectId;
        }
        void CancelConnect(ConnectId) noexcept override {}
        bool StartConnection(const std::shared_ptr<LikesProgram::Net::Connection>& connection) override {
            m_connection = connection.get(); // fake completion 回调固定指向当前连接
            m_readEnabled = true;
            m_stopped = false;
            return true;
        }
        bool StartAccept(LikesProgram::Net::SocketType, AcceptCallback) override { return true; }
        void StopAccept(LikesProgram::Net::SocketType) override {}
        TimeoutId ScheduleTimeout(
            std::chrono::milliseconds delay,
            TimeoutCallback callback) noexcept override {
            if (!m_captureTimeout || !callback) return InvalidTimeoutId;
            m_scheduledDelays.push_back(delay.count());
            m_timeoutCallbacks.push_back(std::move(callback));
            return static_cast<TimeoutId>(m_scheduledDelays.size());
        }
        void CancelTimeout(TimeoutId timeoutId) noexcept override {
            if (timeoutId != InvalidTimeoutId && timeoutId <= m_timeoutCallbacks.size()) {
                m_timeoutCallbacks[static_cast<std::size_t>(timeoutId - 1)] = {};
            }
        }

        // 直接接管调用方 Buffer，记录地址用于证明 payload 未被复制。
        bool QueueWrite(
            LikesProgram::Net::Connection* connection,
            LikesProgram::Net::Buffer&& buffer) override {
            if (m_rejectWrites) return false;
            if (m_write.ReadableBytes() == 0 && m_writeChain.Empty()) m_write = std::move(buffer);
            else m_writeChain.Append(std::move(buffer));
            m_queueWriteCount.fetch_add(1, std::memory_order_acq_rel);
            (void)LikesProgram::Net::Internal::PollerAccess::WriteGrowth(
                *connection,
                PendingWriteBytes(connection)); // 提交 send 前执行统一 completion 背压
            return true;
        }

        // 接管多段写链，验证 TLS 等上层输出无需先合并为单 Buffer。
        bool QueueWrite(
            LikesProgram::Net::Connection* connection,
            LikesProgram::Net::BufferChain&& chain) override {
            if (m_rejectWrites) return false;
            m_writeChain.Append(std::move(chain));
            m_queueWriteCount.fetch_add(1, std::memory_order_acq_rel);
            (void)LikesProgram::Net::Internal::PollerAccess::WriteGrowth(
                *connection,
                PendingWriteBytes(connection)); // TLS chain 与明文 Buffer 复用同一统计
            return true;
        }

        bool QueueDatagramWrite(
            LikesProgram::Net::Connection* connection,
            const LikesProgram::Net::Address& peer,
            LikesProgram::Net::Buffer&& buffer) override {
            if (m_rejectWrites) return false;
            m_datagramPayloads.emplace_back(buffer.AsStringView());
            m_datagramPeers.emplace_back(peer);
            m_datagramCompleted.push_back(false);
            m_datagramPeer = peer;
            m_datagramWrite = std::move(buffer);
            m_queueWriteCount.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t queueCost = m_datagramWrite.ReadableBytes() == 0
                ? 1
                : m_datagramWrite.ReadableBytes(); // 空包也进入背压预算
            (void)LikesProgram::Net::Internal::PollerAccess::WriteGrowth(
                *connection,
                queueCost);
            return true;
        }

        void SetReadEnabled(LikesProgram::Net::Connection* connection, bool enabled) override {
            if (connection == m_connection) m_readEnabled = enabled;
        }
        std::size_t PendingWriteBytes(const LikesProgram::Net::Connection*) const noexcept override {
            return m_write.ReadableBytes() + m_writeChain.ReadableBytes();
        }
        void StopConnection(LikesProgram::Net::Connection* connection) override {
            if (connection != m_connection) return;
            m_stopped = true;
            m_connection = nullptr;
            m_write.RetrieveAll();
            m_writeChain.Clear();
        }
        bool HasPendingShutdownCompletions() const noexcept override {
            return m_shutdownPollsRemaining > 0;
        }
        std::uint64_t CompletedOperationCount() const noexcept override { return 0; }
        std::size_t ProvidedBufferCount() const noexcept override { return 0; }
        std::uint64_t ReadSubmissionCount() const noexcept override { return 0; }
        std::uint64_t AcceptSubmissionCount() const noexcept override { return 0; }
        bool ReceiveBundleEnabled() const noexcept override { return false; }
        LikesProgram::Net::CompletionStats GetCompletionStats() const noexcept override { return {}; }
        const char* BackendName() const noexcept override { return "ownership-test"; }

        const LikesProgram::Net::Buffer& PendingWrite() const noexcept { return m_write; }
        const LikesProgram::Net::Address& DatagramPeer() const noexcept { return m_datagramPeer; }
        const LikesProgram::Net::Buffer& DatagramWrite() const noexcept { return m_datagramWrite; }
        const std::vector<std::string>& DatagramPayloads() const noexcept { return m_datagramPayloads; }
        const std::vector<LikesProgram::Net::Address>& DatagramPeers() const noexcept { return m_datagramPeers; }
        const std::vector<std::int64_t>& ScheduledDelays() const noexcept { return m_scheduledDelays; }
        bool ReadEnabled() const noexcept { return m_readEnabled; }
        bool Stopped() const noexcept { return m_stopped; }
        int QueueWriteCount() const noexcept {
            return m_queueWriteCount.load(std::memory_order_acquire);
        }
        int ChannelAddCount() const noexcept {
            return m_channelAddCount.load(std::memory_order_acquire);
        }
        void EnableTimeoutCapture() noexcept { m_captureTimeout = true; }
        void ArmShutdownDrain(int polls) noexcept { m_shutdownPollsRemaining = polls; }
        int PollCount() const noexcept { return m_pollCount.load(std::memory_order_acquire); }
        void RejectWrites() noexcept { m_rejectWrites = true; }
        bool FireTimeout() {
            for (std::size_t index = 0; index < m_timeoutCallbacks.size(); ++index) {
                if (FireTimeoutAt(index)) return true;
            }
            return false;
        }
        bool FireTimeoutAt(std::size_t index) {
            if (index >= m_timeoutCallbacks.size()) return false;
            TimeoutCallback callback = std::move(m_timeoutCallbacks[index]); // 指定 one-shot timer CQE
            if (!callback) return false;
            callback();
            return true;
        }
        bool CompleteDatagramWriteAt(std::size_t index) {
            if (m_connection == nullptr || index >= m_datagramPayloads.size()
                || m_datagramCompleted[index]) return false;
            m_datagramCompleted[index] = true; // 每个 fake CQE 只回收一次成本
            const std::size_t bytes = m_datagramPayloads[index].size();
            LikesProgram::Net::Internal::PollerAccess::DatagramWriteCompleted(
                *m_connection,
                m_datagramPeers[index],
                bytes == 0 ? 1 : bytes);
            return true;
        }

        // 模拟 partial send CQE，按剩余字节触发 Low，排空后触发 WriteComplete。
        void CompleteWrite(std::size_t completedBytes) {
            LikesProgram::Net::Connection* connection = m_connection; // 回调可能同步关闭并清空指针
            if (connection == nullptr) return;

            const std::size_t fromBuffer = std::min(completedBytes, m_write.ReadableBytes());
            m_write.Consume(fromBuffer);
            completedBytes -= fromBuffer;
            if (completedBytes > 0) m_writeChain.Consume(completedBytes);

            const std::size_t pendingBytes = PendingWriteBytes(connection); // CQE 后准确剩余量
            LikesProgram::Net::Internal::PollerAccess::WriteDrain(*connection, pendingBytes);
            if (pendingBytes == 0) {
                LikesProgram::Net::Internal::PollerAccess::WriteComplete(*connection);
            }
        }

        void CompleteWrite() {
            CompleteWrite(PendingWriteBytes(m_connection));
        }

    private:
        LikesProgram::Net::Connection* m_connection = nullptr; // 当前 fake operation 所属连接
        LikesProgram::Net::Buffer m_write; // 模拟由 CQE 完成前持有的稳定发送 payload
        LikesProgram::Net::BufferChain m_writeChain; // 模拟由 CQE 完成前持有的多段发送链
        LikesProgram::Net::Address m_datagramPeer; // 最近一次显式 datagram 目标 peer
        LikesProgram::Net::Buffer m_datagramWrite{ 0 }; // CQE 前持有的完整 datagram payload
        std::vector<std::string> m_datagramPayloads; // 按提交顺序保存完整数据报内容
        std::vector<LikesProgram::Net::Address> m_datagramPeers; // 按提交顺序保存显式 peer
        std::vector<bool> m_datagramCompleted; // 每个 fake datagram CQE 的单次回收闸门
        std::vector<std::int64_t> m_scheduledDelays; // 按提交顺序保存 fake timer 延迟
        bool m_activationSucceeds = true; // 控制 EventLoop 启动成功或失败
        bool m_readEnabled = true; // 最近一次 SetReadEnabled 状态
        bool m_stopped = false; // StopConnection 是否已进入统一关闭路径
        bool m_captureTimeout = false; // 仅定向测试启用 fake timer completion
        bool m_rejectWrites = false; // 定向模拟 Poller 写提交失败
        int m_shutdownPollsRemaining = 0; // 关闭后仍需回收的模拟 cancel CQE 数
        std::vector<TimeoutCallback> m_timeoutCallbacks; // 按 TimeoutId-1 保存 one-shot callback
        std::atomic<int> m_queueWriteCount{ 0 }; // 实际进入 fake Poller 的 issuer 写次数
        std::atomic<int> m_channelAddCount{ 0 }; // EventLoop wakeup 之外不得为 UDP 添加 Channel
        std::atomic<int> m_pollCount{ 0 }; // 主循环与关闭排空累计 Poll 次数
    };

    class ConnectCompletionLoop final : public LikesProgram::Net::EventLoop {
    public:
        // 测试入口只暴露受保护 Poller connect，不改变产品 EventLoop 公共 API。
        LikesProgram::Net::Poller::ConnectId StartConnect(
            const LikesProgram::Net::Address& remoteAddress,
            LikesProgram::Net::Poller::ConnectCallback callback) noexcept {
            return PollerRef().StartConnect(remoteAddress, std::move(callback));
        }
    };

    class InspectableCompletionEventLoop final : public LikesProgram::Net::EventLoop {
    public:
        // 只读暴露关闭 completion 状态，验证真实 EventLoop 排空后端 operation。
        bool HasPendingShutdownCompletions() {
            return PollerRef().HasPendingShutdownCompletions();
        }
    };

    class OwnershipSendConnection final : public LikesProgram::Net::Connection {
    public:
        // 在 Start 回调栈内移动发送指定 Buffer。
        OwnershipSendConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::Buffer& payload)
            : Connection(fd, loop),
            m_payload(payload) {
        }

    protected:
        // 建连后立即把 payload 所有权转交 completion Poller。
        void OnConnected() override {
            Send(std::move(m_payload));
        }

    private:
        LikesProgram::Net::Buffer& m_payload; // 测试调用方提供的 move-only 发送数据
    };

    class CompletionBackpressureConnection final : public LikesProgram::Net::Connection {
    public:
        // 在同步 OnConnected 栈内完成 fake completion 背压与关闭验收。
        CompletionBackpressureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            OwnershipPoller& poller)
            : Connection(fd, loop),
            m_poller(poller) {
        }

        bool ContractFinished() const noexcept { return m_contractFinished; }

    protected:
        // starting callback 允许 Send/Pause/Resume 直接进入同一 issuer 线程。
        void OnConnected() override {
            LikesProgram::Net::Buffer first; // 首个写链跨越 High 并通过两次 partial CQE 排空
            first.Append("abcdef", 6);
            Send(std::move(first));
            Require(m_highCount == 1 && !m_poller.ReadEnabled(),
                "Completion high watermark should pause multishot read");
            Require(m_poller.PendingWriteBytes(this) == 6,
                "Completion Poller should retain all queued bytes before CQE");

            m_poller.CompleteWrite(4);
            Require(m_lowCount == 1 && m_poller.ReadEnabled(),
                "Completion low watermark should resume multishot read after partial send");
            Require(m_poller.PendingWriteBytes(this) == 2,
                "Partial send should preserve the exact remaining byte count");

            m_poller.CompleteWrite(2);
            Require(m_writeCompleteCount == 1 && m_poller.PendingWriteBytes(this) == 0,
                "Final send completion should release the write chain and notify once");

            SetMaxPendingWriteBytes(5);
            LikesProgram::Net::Buffer overflow; // 超过硬上限后 PollerAccess 立即进入统一关闭流程
            overflow.Append("123456", 6);
            Send(std::move(overflow));
            Require(m_overflowCount == 1 && GetState() == State::Closed && m_poller.Stopped(),
                "Completion hard limit should report overflow and stop the connection");
            m_contractFinished = true;
        }

        void OnWriteHighWatermark(std::size_t) override {
            ++m_highCount;
            PauseReading(); // completion SetReadEnabled(false) 必须取消长期 read
        }

        void OnWriteLowWatermark(std::size_t) override {
            ++m_lowCount;
            ResumeReading(); // completion SetReadEnabled(true) 必须重提 read
        }

        void OnWriteQueueOverflow(std::size_t) override {
            ++m_overflowCount;
        }

        void OnWriteComplete() override {
            ++m_writeCompleteCount;
        }

    private:
        OwnershipPoller& m_poller; // 同步 fake CQE 驱动器
        int m_highCount = 0; // High 回调次数
        int m_lowCount = 0; // Low 回调次数
        int m_overflowCount = 0; // Overflow 回调次数
        int m_writeCompleteCount = 0; // 写链排空通知次数
        bool m_contractFinished = false; // OnConnected 内全部断言已完成
    };

    class CompletionShutdownConnection final : public LikesProgram::Net::Connection {
    public:
        // 在 issuer 回调内验证 Shutdown 写闸门和排空后的半关闭语义。
        CompletionShutdownConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            OwnershipPoller& poller)
            : Connection(fd, loop),
            m_poller(poller) {
        }

        bool ContractFinished() const noexcept { return m_contractFinished; }

    protected:
        void OnConnected() override {
            LikesProgram::Net::Buffer queued; // Shutdown 前已经接受的应用写必须继续排空
            queued.Append("abcdef", 6);
            Send(std::move(queued));
            Require(m_poller.PendingWriteBytes(this) == 6,
                "Graceful shutdown should begin with a pending completion write");

            Shutdown();
            Require(GetState() == State::Closing,
                "Shutdown in issuer callback should enter Closing immediately");

            LikesProgram::Net::Buffer rejected; // Closing 后的新应用写不得越过写闸门
            rejected.Append("late", 4);
            Send(std::move(rejected));
            Require(m_poller.PendingWriteBytes(this) == 6,
                "Closing connection should reject application writes after Shutdown");

            m_poller.CompleteWrite(6);
            Require(m_writeCompleteCount == 1
                    && m_poller.PendingWriteBytes(this) == 0
                    && GetState() == State::Closing,
                "Graceful shutdown should drain accepted bytes before waiting for peer EOF");

            LikesProgram::Net::Internal::PollerAccess::PeerClosed(*this);
            Require(GetState() == State::Closed && m_poller.Stopped(),
                "Peer EOF after write shutdown should complete the unified close path");
            m_contractFinished = true;
        }

        void OnWriteComplete() override {
            ++m_writeCompleteCount;
        }

    private:
        OwnershipPoller& m_poller; // 同步 fake completion 驱动器
        int m_writeCompleteCount = 0; // Shutdown 前写链排空通知次数
        bool m_contractFinished = false; // 完整关闭契约已执行
    };

    struct RealBackpressureState {
        std::atomic<int> m_connected{ 0 };     // 真实 Connection 已进入可发送状态
        std::atomic<int> m_highCount{ 0 };     // 写队列首次跨过高水位
        std::atomic<int> m_lowCount{ 0 };      // partial CQE 排空后跨过低水位
        std::atomic<int> m_writeCompleteCount{ 0 }; // 全部已接受写完成次数
        std::atomic<int> m_closedCount{ 0 };   // peer EOF 后统一关闭次数
    };

    class RealBackpressureConnection final : public LikesProgram::Net::Connection {
    public:
        // 使用真实 io_uring send CQE 观察背压与 graceful shutdown 顺序。
        RealBackpressureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            RealBackpressureState& state)
            : Connection(fd, loop),
            m_state(state) {
        }

    protected:
        // 发布连接已启动，测试线程随后登记 Send 与 Shutdown。
        void OnConnected() override {
            m_state.m_connected.fetch_add(1, std::memory_order_release);
        }

        // 高水位暂停长期 read，验证真实 Poller cancel 路径。
        void OnWriteHighWatermark(std::size_t) override {
            m_state.m_highCount.fetch_add(1, std::memory_order_release);
            PauseReading();
        }

        // partial send 排空到低水位后恢复 read，允许最终接收 peer EOF。
        void OnWriteLowWatermark(std::size_t) override {
            m_state.m_lowCount.fetch_add(1, std::memory_order_release);
            ResumeReading();
        }

        // 整条写链排空后只通知一次。
        void OnWriteComplete() override {
            m_state.m_writeCompleteCount.fetch_add(1, std::memory_order_release);
        }

        // 记录 peer EOF 驱动的最终统一关闭。
        void OnClosed() override {
            m_state.m_closedCount.fetch_add(1, std::memory_order_release);
        }

    private:
        RealBackpressureState& m_state; // issuer 回调写入、测试线程原子读取
    };

    class StartingCallbackBarrierConnection final : public LikesProgram::Net::Connection {
    public:
        StartingCallbackBarrierConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::mutex& mutex,
            std::condition_variable& condition,
            bool& entered,
            bool& released)
            : Connection(fd, loop),
            m_mutex(mutex),
            m_condition(condition),
            m_entered(entered),
            m_released(released) {
        }

    protected:
        // 阻塞 Start 回调，给其他线程制造稳定的 startingCallbacks 窗口。
        void OnConnected() override {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_entered = true;
            m_condition.notify_all();
            m_condition.wait(lock, [this]() { return m_released; });
        }

    private:
        std::mutex& m_mutex;                       // 回调屏障互斥量
        std::condition_variable& m_condition;      // 启动/释放通知
        bool& m_entered;                           // OnConnected 已进入
        bool& m_released;                          // 允许 OnConnected 返回
    };

    class ThrowingStartingCallbackConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingStartingCallbackConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 验证异常离开 Start 时必须撤销 startingCallbacks 快路径资格。
        void OnConnected() override {
            throw std::runtime_error("intentional starting callback failure");
        }
    };

    class ThrowingAcceptedConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingAcceptedConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 模拟用户 accepted connection 在启动回调中失败。
        void OnConnected() override {
            throw std::runtime_error("intentional accepted connection failure");
        }
    };

    class ThrowingCompletionCallbacksConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingCompletionCallbacksConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 模拟业务读回调异常，必须在 completion Connection 边界内收敛。
        void OnMessage(LikesProgram::Net::Buffer&) override {
            throw std::runtime_error("intentional completion message failure");
        }

        // 关闭前通知异常不能阻断 Poller cancel 与 buffer lease 归还。
        void OnClosing() override {
            throw std::runtime_error("intentional completion closing failure");
        }

        // 最终关闭通知异常也不能重新击穿 issuer 主循环。
        void OnClosed() override {
            throw std::runtime_error("intentional completion closed failure");
        }
    };

    class ThrowingLowWatermarkConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingLowWatermarkConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 模拟 partial write CQE 解除背压时用户通知失败。
        void OnWriteLowWatermark(std::size_t) override {
            throw std::runtime_error("intentional low watermark failure");
        }
    };

    class ThrowingWriteErrorConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingWriteErrorConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 模拟 Poller 拒绝写入后的用户错误观察者失败。
        void OnError(int) override {
            throw std::runtime_error("intentional write submission observer failure");
        }
    };

    struct QueuedTlsShutdownState {
        int m_sequence = 0;                // issuer 内 Engine 调用顺序
        int m_plaintextOrder = 0;          // ConsumePlaintext 的顺序号
        int m_shutdownOrder = 0;           // Shutdown 的顺序号
        int m_shutdownCount = 0;           // close_notify 生成次数
    };

    class QueuedTlsShutdownEngine final : public LikesProgram::Net::TlsEngine {
    public:
        explicit QueuedTlsShutdownEngine(QueuedTlsShutdownState& state)
            : m_state(state) {
        }

        // 测试 Engine 直接进入 Active，隔离本用例关注的应用写/close_notify 顺序。
        LikesProgram::Net::TlsResult StartHandshake(LikesProgram::Net::BufferChain&) override {
            m_tlsState = LikesProgram::Net::TlsState::Active;
            return {};
        }

        // 本用例不接收 socket 密文，保留完整 Engine 契约入口。
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& ciphertextInput,
            LikesProgram::Net::BufferChain&,
            LikesProgram::Net::BufferChain&) override {
            ciphertextInput.Consume(ciphertextInput.ReadableBytes());
            return {};
        }

        // 记录已接受明文先进入 Engine，并原样产生测试密文。
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain& plaintextInput,
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            m_state.m_plaintextOrder = ++m_state.m_sequence;
            LikesProgram::Net::Buffer ciphertext; // 测试 Engine 原样汇总各明文段
            const std::size_t segmentCount = plaintextInput.SegmentCount(); // 当前排队明文段数
            for (std::size_t index = 0; index < segmentCount; ++index) {
                const LikesProgram::Net::BufferSlice segment = plaintextInput.Segment(index);
                ciphertext.Append(segment.Data(), segment.Size());
            }
            plaintextInput.Clear();
            ciphertextOutput.Append(std::move(ciphertext));
            return { LikesProgram::Net::TlsAction::CiphertextReady, 0 };
        }

        // close_notify 必须只生成一次且排在已接受明文之后。
        LikesProgram::Net::TlsResult Shutdown(
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            m_state.m_shutdownOrder = ++m_state.m_sequence;
            ++m_state.m_shutdownCount;
            LikesProgram::Net::Buffer closeNotify;
            closeNotify.Append("close", 5);
            ciphertextOutput.Append(std::move(closeNotify));
            m_tlsState = LikesProgram::Net::TlsState::Closed;
            return {
                LikesProgram::Net::TlsAction::CiphertextReady
                    | LikesProgram::Net::TlsAction::CloseTransport,
                0
            };
        }

        LikesProgram::Net::TlsState State() const noexcept override {
            return m_tlsState;
        }

        const char* NegotiatedProtocol() const noexcept override {
            return "";
        }

    private:
        QueuedTlsShutdownState& m_state; // 单 issuer 测试状态
        LikesProgram::Net::TlsState m_tlsState = LikesProgram::Net::TlsState::Handshaking; // Engine 生命周期
    };

    class ThrowingOutboundTlsEngine final : public LikesProgram::Net::TlsEngine {
    public:
        explicit ThrowingOutboundTlsEngine(bool throwOnShutdown)
            : m_throwOnShutdown(throwOnShutdown) {
        }

        // 测试直接进入 Active，隔离出站明文与关闭异常边界。
        LikesProgram::Net::TlsResult StartHandshake(LikesProgram::Net::BufferChain&) override {
            m_state = LikesProgram::Net::TlsState::Active;
            return {};
        }

        // 本用例不接收密文，保持 Engine 抽象契约完整。
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& ciphertextInput,
            LikesProgram::Net::BufferChain&,
            LikesProgram::Net::BufferChain&) override {
            ciphertextInput.Clear();
            return {};
        }

        // 发送用例在这里抛出，连接不得保持假 Connected。
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain&,
            LikesProgram::Net::BufferChain&) override {
            throw std::runtime_error("intentional outbound TLS failure");
        }

        // 关闭用例在 close_notify 生成点抛出，连接不得永久 Closing。
        LikesProgram::Net::TlsResult Shutdown(LikesProgram::Net::BufferChain&) override {
            if (m_throwOnShutdown) {
                throw std::runtime_error("intentional TLS shutdown failure");
            }
            m_state = LikesProgram::Net::TlsState::Closed;
            return { LikesProgram::Net::TlsAction::CloseTransport, 0 };
        }

        LikesProgram::Net::TlsState State() const noexcept override { return m_state; }
        const char* NegotiatedProtocol() const noexcept override { return ""; }

    private:
        bool m_throwOnShutdown = false; // 选择本次回归的异常注入点
        LikesProgram::Net::TlsState m_state = LikesProgram::Net::TlsState::Handshaking; // 测试 Engine 状态
    };

    class ImmediateThrowingTlsConnection final : public LikesProgram::Net::Connection {
    public:
        ImmediateThrowingTlsConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            bool throwOnShutdown)
            : Connection(fd, loop),
            m_throwOnShutdown(throwOnShutdown) {
        }

    protected:
        // 在 Start issuer 回调内同步安装 Engine，避免测试依赖额外启动时序。
        void OnConnected() override {
            SetTlsEngineFactory(LikesProgram::Net::TlsEngineFactory([this]() {
                return std::make_unique<ThrowingOutboundTlsEngine>(m_throwOnShutdown);
            }));
            UpgradeCommunication();
        }

    private:
        bool m_throwOnShutdown = false; // 每连接选择发送或关闭异常模式
    };

    class StalledTlsEngine final : public LikesProgram::Net::TlsEngine {
    public:
        // 不产生握手输出并保持 Handshaking，等待 fake timer 到期。
        LikesProgram::Net::TlsResult StartHandshake(LikesProgram::Net::BufferChain&) override { return {}; }
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& input,
            LikesProgram::Net::BufferChain&,
            LikesProgram::Net::BufferChain&) override {
            input.Clear();
            return {};
        }
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain& input,
            LikesProgram::Net::BufferChain&) override {
            input.Clear();
            return {};
        }
        LikesProgram::Net::TlsResult Shutdown(LikesProgram::Net::BufferChain&) override {
            return { LikesProgram::Net::TlsAction::CloseTransport, 0 };
        }
        LikesProgram::Net::TlsState State() const noexcept override {
            return LikesProgram::Net::TlsState::Handshaking;
        }
        const char* NegotiatedProtocol() const noexcept override { return ""; }
    };

    class ThrowingTlsTimeoutConnection final : public LikesProgram::Net::Connection {
    public:
        ThrowingTlsTimeoutConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 在启动 issuer 内进入永不完成的握手，确保 timer 被登记。
        void OnConnected() override {
            SetTlsEngineFactory(LikesProgram::Net::TlsEngineFactory([]() {
                return std::make_unique<StalledTlsEngine>();
            }));
            UpgradeCommunication();
        }
        // 模拟用户错误观察者失败，关闭逻辑仍必须继续。
        void OnError(int) override {
            throw std::runtime_error("intentional TLS timeout observer failure");
        }
    };

    // 模拟非 issuer Send 后立即 Shutdown，已接受 Buffer 必须先进入 completion 写链。
    void TestQueuedSendBeforeShutdownDrains() {
        auto poller = std::make_unique<OwnershipPoller>(); // 保留 fake completion 驱动入口
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd =
            LikesProgram::Net::kInvalidSocket - 1; // 系统调用必定拒绝且不会误关真实描述符
        auto connection = std::make_shared<LikesProgram::Net::Connection>(fakeFd, &loop);
        loop.AttachConnection(connection);
        connection->Start();

        LikesProgram::Net::Buffer payload; // loop 尚未 Start，Send 必然成为跨线程排队任务
        payload.Append("accepted", 8);
        connection->Send(std::move(payload));
        connection->Shutdown();
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closing,
            "Queued shutdown should publish Closing immediately");

        loop.Shutdown();
        loop.Start(); // shutdown-before-start 路径仍必须按 FIFO 消费已登记 Send 与 Shutdown
        Require(pollerAddress->PendingWriteBytes(connection.get()) == 8,
            "Shutdown must not discard a Buffer accepted before Closing");

        pollerAddress->CompleteWrite();
        Require(pollerAddress->PendingWriteBytes(connection.get()) == 0,
            "Accepted queued Buffer should drain through the completion write owner");
        LikesProgram::Net::Internal::PollerAccess::PeerClosed(*connection);
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Peer EOF should finish queued graceful shutdown");
    }

    // TLS graceful shutdown 必须等待排队明文进入 Engine，再追加唯一 close_notify。
    void TestQueuedTlsSendPrecedesCloseNotify() {
        auto poller = std::make_unique<OwnershipPoller>(); // 同一写链观察应用密文与 close_notify
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<LikesProgram::Net::Connection>(fakeFd, &loop);
        QueuedTlsShutdownState state; // 所有 Engine 调用均由本轮 ProcessPendingTasks 串行执行
        connection->SetTlsEngineFactory(LikesProgram::Net::TlsEngineFactory([&state]() {
            return std::make_unique<QueuedTlsShutdownEngine>(state);
        }));
        loop.AttachConnection(connection);
        connection->Start();
        connection->UpgradeCommunication(); // 先排队创建 Engine，再排队应用明文

        LikesProgram::Net::Buffer payload;
        payload.Append("accepted", 8);
        connection->Send(std::move(payload));
        connection->Shutdown();
        loop.Shutdown();
        loop.Start();

        Require(state.m_plaintextOrder == 1 && state.m_shutdownOrder == 2,
            "TLS Engine should consume accepted plaintext before Shutdown");
        Require(state.m_shutdownCount == 1,
            "TLS graceful shutdown should generate close_notify exactly once");
        Require(pollerAddress->PendingWriteBytes(connection.get()) == 13,
            "TLS completion chain should retain application ciphertext before close_notify");
        pollerAddress->CompleteWrite();
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "TLS close_notify completion should close the connection");
    }

    void TestOutboundTlsEngineExceptionsCloseConnection() {
        for (bool throwOnShutdown : { false, true }) {
            auto poller = std::make_unique<OwnershipPoller>(); // fake Poller 观察 Engine 异常后的 StopConnection
            OwnershipPoller* pollerAddress = poller.get();
            LikesProgram::Net::EventLoop loop(std::move(poller));
            const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
            auto connection = std::make_shared<ImmediateThrowingTlsConnection>(
                fakeFd,
                &loop,
                throwOnShutdown);
            loop.AttachConnection(connection);
            connection->Start();

            if (throwOnShutdown) {
                connection->Shutdown();
            }
            else {
                LikesProgram::Net::Buffer payload; // 出站明文在 issuer 任务中进入 Engine
                payload.Append("fail", 4);
                connection->Send(std::move(payload));
            }
            loop.Shutdown();
            loop.Start();

            Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
                "Outbound TLS Engine exceptions must converge the connection to Closed");
            Require(pollerAddress->Stopped(),
                "Outbound TLS Engine exceptions should cancel Poller operations");
        }
    }

    void TestTlsTimeoutObserverExceptionStillClosesConnection() {
        auto poller = std::make_unique<OwnershipPoller>(); // 按需启用 fake timer completion
        OwnershipPoller* pollerAddress = poller.get();
        pollerAddress->EnableTimeoutCapture();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<ThrowingTlsTimeoutConnection>(fakeFd, &loop);
        loop.AttachConnection(connection);
        connection->Start();

        bool escaped = false; // io_uring timer callback 不得看到用户 OnError 异常
        try {
            Require(pollerAddress->FireTimeout(),
                "Stalled TLS handshake should register a timeout completion");
        }
        catch (...) {
            escaped = true;
        }

        Require(!escaped,
            "TLS timeout observer exceptions must not escape the timer completion boundary");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "TLS timeout must close even when OnError throws");
        Require(pollerAddress->Stopped(),
            "TLS timeout observer failure should still cancel connection operations");

        loop.Shutdown();
        loop.Start(); // 消费关闭路径的延迟 self 释放任务
    }

    // 其他线程不得借用 startingCallbacks 快路径直接调用 single-issuer Poller。
    void TestStartingCallbackFastPathRequiresSameThread() {
        auto poller = std::make_unique<OwnershipPoller>(); // 记录真正进入 Poller 的写次数
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        std::mutex barrierMutex; // 阻塞 OnConnected 的测试屏障
        std::condition_variable barrierCondition; // 双线程进入/释放通知
        bool entered = false; // 启动线程是否进入 OnConnected
        bool released = false; // 是否允许启动线程返回
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<StartingCallbackBarrierConnection>(
            fakeFd,
            &loop,
            barrierMutex,
            barrierCondition,
            entered,
            released);
        loop.AttachConnection(connection);

        std::thread startingThread([connection]() { connection->Start(); });
        {
            std::unique_lock<std::mutex> lock(barrierMutex);
            barrierCondition.wait(lock, [&entered]() { return entered; });
        }

        LikesProgram::Net::Buffer payload; // 当前线程不是 Start 回调线程，只能排队
        payload.Append("external", 8);
        connection->Send(std::move(payload));
        Require(pollerAddress->QueueWriteCount() == 0,
            "External thread must not enter Poller through startingCallbacks fast path");

        {
            std::lock_guard<std::mutex> lock(barrierMutex);
            released = true;
            barrierCondition.notify_all();
        }
        startingThread.join();

        loop.Shutdown();
        loop.Start();
        Require(pollerAddress->QueueWriteCount() == 1
                && pollerAddress->PendingWriteBytes(connection.get()) == 8,
            "External send should enter Poller only when EventLoop consumes its task");
        pollerAddress->CompleteWrite();
        LikesProgram::Net::Internal::PollerAccess::PeerClosed(*connection);
    }

    // OnConnected 抛出后，同一线程的后续 Send 也必须重新排队到 EventLoop。
    void TestStartingCallbackFlagClearsOnException() {
        auto poller = std::make_unique<OwnershipPoller>(); // 观察异常后的发送是否仍绕过队列
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<ThrowingStartingCallbackConnection>(fakeFd, &loop);
        loop.AttachConnection(connection);

        bool callbackFailed = false; // Start 应把用户回调异常传播给当前 issuer 测试入口
        try {
            connection->Start();
        }
        catch (const std::runtime_error&) {
            callbackFailed = true;
        }
        Require(callbackFailed, "Throwing OnConnected should leave Start through exception");

        LikesProgram::Net::Buffer payload; // 与失败 Start 同线程，仍不能保留过期快路径资格
        payload.Append("after-failure", 13);
        connection->Send(std::move(payload));
        Require(pollerAddress->QueueWriteCount() == 0,
            "Starting callback flag should clear before an exception escapes Start");

        loop.Shutdown();
        loop.Start();
        Require(pollerAddress->QueueWriteCount() == 1,
            "Post-failure Send should enter Poller through the EventLoop task queue");
        pollerAddress->CompleteWrite();
        LikesProgram::Net::Internal::PollerAccess::PeerClosed(*connection);
    }

    class EchoConnection final : public LikesProgram::Net::Connection {
    public:
        // 按协议创建回显连接，不向测试业务暴露 Transport 实现。
        EchoConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::TransportKind kind = LikesProgram::Net::TransportKind::Tcp)
            : Connection(fd, loop, kind) {
        }

    protected:
        // 收到任意数据后原样写回，并消费输入缓冲。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            Send(in.Peek(), in.ReadableBytes());
            in.RetrieveAll();
        }
    };

    class ConnectedDatagramSendConnection final : public LikesProgram::Net::Connection {
    public:
        // 创建已 connect 的 UDP 连接，并在启动回调中走无显式 peer 的发送路径。
        ConnectedDatagramSendConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp) {
        }

    protected:
        // 首次业务发送必须复用 socket 已连接对端，不退化为 sendmsg 显式地址。
        void OnConnected() override {
            Send("ping", 4);
        }
    };

    struct DtlsClientStartupState {
        int m_onSecureLayerReady = 0; // 安全层入口调用次数
        int m_onConnected = 0;        // UDP 建连通知次数
        int m_createdEngines = 0;     // Factory 创建的每 peer Engine 数
        int m_lastError = 0;          // 启动失败时的 peer-aware 错误
        std::vector<std::string> m_order; // issuer 线程观察到的启动顺序
    };

    class DtlsClientStartupEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        explicit DtlsClientStartupEngine(DtlsClientStartupState& state)
            : m_state(state) {
        }

        // 生成固定 client flight，并登记 Engine 重传 timer。
        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            m_state.m_order.emplace_back("initial-flight");
            LikesProgram::Net::Buffer flight(0); // 完整 client 握手数据报
            flight.Append("client-flight", 13);
            ciphertextOutput.Append(std::move(flight));
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::ArmRetransmitTimer,
                0,
                25
            };
        }

        // 启动契约不推进入站握手，只满足 Engine 完整接口。
        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch&,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            input.RetrieveAll();
            return {};
        }

        // Handshaking 状态不接受应用明文。
        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            input.RetrieveAll();
            return {};
        }

        // 启动契约只记录 timer，不主动触发回调。
        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            return {};
        }

        // 测试清理时直接进入 Closed，不生成额外数据报。
        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            m_engineState = LikesProgram::Net::DtlsState::Closed;
            return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
        }

        LikesProgram::Net::DtlsState State() const noexcept override { return m_engineState; }
        const char* NegotiatedProtocol() const noexcept override { return ""; }

    private:
        DtlsClientStartupState& m_state; // 启动顺序观察状态
        LikesProgram::Net::DtlsState m_engineState = LikesProgram::Net::DtlsState::Handshaking; // 当前握手态
    };

    class DtlsClientStartupConnection final : public LikesProgram::Net::Connection {
    public:
        DtlsClientStartupConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            DtlsClientStartupState& state)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_state(state) {
            SetDtlsEngineFactory(LikesProgram::Net::DtlsEngineFactory(
                LikesProgram::Net::DtlsRole::Client,
                [this](
                    const LikesProgram::Net::Address&,
                    const LikesProgram::Net::Address&,
                    std::size_t maximumBytes) {
                    Require(maximumBytes == 1200, "DTLS client Factory should receive the exact MTU");
                    ++m_state.m_createdEngines;
                    m_state.m_order.emplace_back("factory-create");
                    return std::make_unique<DtlsClientStartupEngine>(m_state);
                }));
        }

    protected:
        void OnSecureLayerReady() override {
            ++m_state.m_onSecureLayerReady;
            m_state.m_order.emplace_back("secure-ready");
        }

        void OnConnected() override {
            ++m_state.m_onConnected;
            m_state.m_order.emplace_back("connected");
        }

        void OnDtlsSessionError(const LikesProgram::Net::Address&, int error) override {
            m_state.m_lastError = error;
        }

    private:
        DtlsClientStartupState& m_state; // issuer 线程启动观察状态
    };

    struct RealDtlsClientState {
        std::atomic<int> m_handshakeCallbacks{ 0 }; // Active 单次通知
        std::atomic<int> m_retransmitCallbacks{ 0 }; // 真实 timeout CQE 次数
        std::atomic<int> m_sessionClosedCallbacks{ 0 }; // close ciphertext drain 次数
        std::atomic<bool> m_connectionClosed{ false }; // connected session 关闭后 Connection 状态
        std::atomic<int> m_lastError{ 0 }; // 最近 DTLS/Connection error
        std::mutex m_mutex; // 保护真实 loop 与测试线程的明文/peer 快照
        std::vector<std::string> m_plaintext; // 每个 Engine plaintext 元素对应一次回调
        std::vector<std::string> m_plaintextPeers; // 每次明文回调的精确 peer
    };

    class RealDtlsClientEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        explicit RealDtlsClientEngine(RealDtlsClientState& state)
            : m_state(state) {
        }

        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            AppendDatagram(ciphertextOutput, "client-flight-1");
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::ArmRetransmitTimer,
                0,
                20
            };
        }

        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch& plaintextOutput,
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            const std::string payload(input.AsStringView()); // 真实 UDP ciphertext 数据报
            input.RetrieveAll();
            if (payload == "cookie-response") {
                m_engineState = LikesProgram::Net::DtlsState::Active;
                AppendDatagram(ciphertextOutput, "client-flight-2a");
                AppendDatagram(ciphertextOutput, "client-flight-2b");
                return {
                    LikesProgram::Net::DtlsAction::CiphertextReady
                        | LikesProgram::Net::DtlsAction::CancelRetransmitTimer,
                    0,
                    0
                };
            }
            if (payload == "server-data") {
                AppendDatagram(plaintextOutput, "plain-one");
                AppendDatagram(plaintextOutput, "plain-two");
                return { LikesProgram::Net::DtlsAction::PlaintextReady, 0, 0 };
            }
            return {};
        }

        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            input.RetrieveAll();
            AppendDatagram(ciphertextOutput, "app-flight-1");
            AppendDatagram(ciphertextOutput, "app-flight-2");
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            m_state.m_retransmitCallbacks.fetch_add(1, std::memory_order_release);
            AppendDatagram(ciphertextOutput, "client-flight-retry");
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            m_engineState = LikesProgram::Net::DtlsState::Closed;
            AppendDatagram(ciphertextOutput, "client-close");
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::CloseSession,
                0,
                0
            };
        }

        LikesProgram::Net::DtlsState State() const noexcept override { return m_engineState; }
        const char* NegotiatedProtocol() const noexcept override { return "real-client-probe"; }

    private:
        static void AppendDatagram(
            LikesProgram::Net::DtlsDatagramBatch& output,
            const char* bytes) {
            LikesProgram::Net::Buffer datagram(0); // 单个完整 Engine 输出数据报
            datagram.Append(bytes, std::strlen(bytes));
            output.Append(std::move(datagram));
        }

        RealDtlsClientState& m_state; // 真实 timeout CQE 观察状态
        LikesProgram::Net::DtlsState m_engineState = LikesProgram::Net::DtlsState::Handshaking; // client 状态
    };

    class RealDtlsClientConnection final : public LikesProgram::Net::Connection {
    public:
        RealDtlsClientConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            RealDtlsClientState& state)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_state(state) {
            SetDtlsEngineFactory(LikesProgram::Net::DtlsEngineFactory(
                LikesProgram::Net::DtlsRole::Client,
                [this](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t) {
                    return std::make_unique<RealDtlsClientEngine>(m_state);
                }));
            SetDtlsHandshakeTimeout(std::chrono::seconds(2));
            SetDtlsSessionIdleTimeout(std::chrono::milliseconds(0));
        }

    protected:
        void OnDtlsHandshakeDone(const LikesProgram::Net::Address&, const char*) override {
            m_state.m_handshakeCallbacks.fetch_add(1, std::memory_order_release);
        }

        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address& peer,
            std::size_t,
            bool) override {
            std::lock_guard<std::mutex> lock(m_state.m_mutex);
            m_state.m_plaintext.emplace_back(input.AsStringView());
            m_state.m_plaintextPeers.emplace_back(peer.ToString());
            input.RetrieveAll();
        }

        void OnDtlsSessionError(const LikesProgram::Net::Address&, int error) override {
            m_state.m_lastError.store(error, std::memory_order_release);
        }

        void OnDtlsSessionClosed(const LikesProgram::Net::Address&) override {
            m_state.m_sessionClosedCallbacks.fetch_add(1, std::memory_order_release);
        }

        void OnError(int error) override {
            m_state.m_lastError.store(error, std::memory_order_release);
        }

        void OnClosed() override {
            m_state.m_connectionClosed.store(true, std::memory_order_release);
        }

    private:
        RealDtlsClientState& m_state; // loop 与测试线程共享的真实路径状态
    };

    struct DtlsServerRoutingState {
        std::unordered_map<std::string, int> m_createdByPeer; // 每个 canonical 测试 peer 的 Engine 数
        std::unordered_map<std::string, std::string> m_plaintextByPeer; // 每 peer 唯一明文
        std::unordered_map<std::string, int> m_plaintextCallbacks; // duplicate 不得增加的交付次数
        std::unordered_map<std::string, int> m_handshakeCallbacks; // 每 peer Active 单次通知
        std::unordered_map<std::string, int> m_sessionErrors; // peer-local fatal error 次数
        std::unordered_map<std::string, int> m_lastSessionError; // 每 peer 最近稳定错误码
        std::unordered_map<std::string, int> m_retransmitCallbacks; // Engine timer 到期次数
        std::unordered_map<std::string, int> m_closedCallbacks; // ciphertext drain 后关闭通知次数
        std::atomic<int> m_totalPlaintextCallbacks{ 0 }; // 真实 loop 的业务交付完成标记
        std::string m_throwPlaintextPeer; // 指定业务回调异常注入 peer
        int m_onConnected = 0; // 共享 UDP Connection 建立次数
    };

    class DtlsServerRoutingEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        DtlsServerRoutingEngine(std::string peerKey, DtlsServerRoutingState& state)
            : m_peerKey(std::move(peerKey)),
            m_state(state) {
        }

        // Server 不主动生成首个 flight。
        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            return {};
        }

        // 模拟 cookie、乱序 fragment、duplicate application data 与 peer close。
        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch& plaintextOutput,
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            const std::string payload(input.AsStringView()); // 本次完整 ciphertext 数据报
            if (payload == "unconsumed") return {}; // 故意违反完整输入消费契约
            input.RetrieveAll();
            if (payload == "cookie") {
                LikesProgram::Net::Buffer challenge(0); // peer 专属 cookie challenge
                const std::string bytes = "challenge:" + m_peerKey;
                challenge.Append(bytes.data(), bytes.size());
                ciphertextOutput.Append(std::move(challenge));
                return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
            }
            if (payload == "valid-cookie") {
                m_engineState = LikesProgram::Net::DtlsState::Active;
                LikesProgram::Net::Buffer flight(0); // peer 专属完成 flight
                const std::string bytes = "active:" + m_peerKey;
                flight.Append(bytes.data(), bytes.size());
                ciphertextOutput.Append(std::move(flight));
                return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
            }
            if (payload == "fragment-2") {
                m_sawFragmentTwo = true;
                return {};
            }
            if (payload == "fragment-1") {
                m_sawFragmentOne = true;
                return {};
            }
            if (payload == "malformed") {
                return {
                    LikesProgram::Net::DtlsAction::ArmRetransmitTimer
                        | LikesProgram::Net::DtlsAction::CancelRetransmitTimer,
                    0,
                    25
                };
            }
            if (payload == "arm") {
                return { LikesProgram::Net::DtlsAction::ArmRetransmitTimer, 0, 10 };
            }
            if (payload == "mtu-1200" || payload == "mtu-1201") {
                const std::size_t bytes = payload == "mtu-1200" ? 1200 : 1201; // 精确 MTU 边界
                std::string storage(bytes, 'm');
                LikesProgram::Net::Buffer output(0);
                output.Append(storage.data(), storage.size());
                ciphertextOutput.Append(std::move(output));
                return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
            }
            if (payload == "multi-flight") {
                for (int index = 0; index < 4; ++index) {
                    LikesProgram::Net::Buffer output(0); // 同一 Engine 调用的 linked send 候选
                    const std::string bytes = "multi:" + m_peerKey + ":" + std::to_string(index);
                    output.Append(bytes.data(), bytes.size());
                    ciphertextOutput.Append(std::move(output));
                }
                return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
            }
            if (payload.rfind("data:", 0) == 0 && m_engineState == LikesProgram::Net::DtlsState::Active) {
                if (payload == m_lastDeliveredCiphertext) return {}; // duplicate/replay 不重复交付
                m_lastDeliveredCiphertext = payload;
                const std::string plaintext = payload.substr(5); // 测试业务明文
                LikesProgram::Net::Buffer output(0);
                output.Append(plaintext.data(), plaintext.size());
                plaintextOutput.Append(std::move(output));
                return { LikesProgram::Net::DtlsAction::PlaintextReady, 0, 0 };
            }
            if (payload == "close") {
                m_engineState = LikesProgram::Net::DtlsState::Closed;
                return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
            }
            if (payload == "close-flight") {
                m_engineState = LikesProgram::Net::DtlsState::Closed;
                LikesProgram::Net::Buffer output(0); // 必须先排队的 peer-local close flight
                output.Append("close-flight", 12);
                ciphertextOutput.Append(std::move(output));
                return {
                    LikesProgram::Net::DtlsAction::CiphertextReady
                        | LikesProgram::Net::DtlsAction::CloseSession,
                    0,
                    0
                };
            }
            return {};
        }

        // Active Server 业务发送原样包装为 peer 专属 ciphertext。
        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& input,
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            const std::string plaintext(input.AsStringView()); // 单个业务数据报
            input.RetrieveAll();
            LikesProgram::Net::Buffer output(0);
            const std::string bytes = m_peerKey + ":" + plaintext;
            output.Append(bytes.data(), bytes.size());
            ciphertextOutput.Append(std::move(output));
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            ++m_state.m_retransmitCallbacks[m_peerKey];
            LikesProgram::Net::Buffer output(0); // 固定 peer 的重传 flight
            const std::string bytes = "retransmit:" + m_peerKey;
            output.Append(bytes.data(), bytes.size());
            ciphertextOutput.Append(std::move(output));
            return { LikesProgram::Net::DtlsAction::CiphertextReady, 0, 0 };
        }

        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            m_engineState = LikesProgram::Net::DtlsState::Closed;
            return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
        }

        LikesProgram::Net::DtlsState State() const noexcept override { return m_engineState; }
        const char* NegotiatedProtocol() const noexcept override { return "server-probe"; }

    private:
        std::string m_peerKey; // Factory 传入的稳定 peer 标识，仅供测试输出
        DtlsServerRoutingState& m_state; // timer 到期观察状态
        LikesProgram::Net::DtlsState m_engineState = LikesProgram::Net::DtlsState::Handshaking; // peer 状态
        bool m_sawFragmentOne = false; // 当前 peer 的 fragment-1 状态
        bool m_sawFragmentTwo = false; // 当前 peer 的 fragment-2 状态
        std::string m_lastDeliveredCiphertext; // 最近一次已交付 record，用于 duplicate/replay
    };

    class DtlsServerRoutingConnection final : public LikesProgram::Net::Connection {
    public:
        DtlsServerRoutingConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            DtlsServerRoutingState& state,
            std::size_t maximumSessions = 0)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_state(state) {
            SetDtlsEngineFactory(LikesProgram::Net::DtlsEngineFactory(
                LikesProgram::Net::DtlsRole::Server,
                [this](
                    const LikesProgram::Net::Address& peer,
                    const LikesProgram::Net::Address&,
                    std::size_t) {
                    const std::string key = peer.ToString(); // 测试观察键，不参与产品路由
                    ++m_state.m_createdByPeer[key];
                    return std::make_unique<DtlsServerRoutingEngine>(key, m_state);
                }));
            if (maximumSessions != 0) {
                SetDtlsSessionLimits(maximumSessions, maximumSessions, 256 * 1024);
            }
        }

    protected:
        void OnConnected() override { ++m_state.m_onConnected; }

        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address& peer,
            std::size_t,
            bool) override {
            const std::string key = peer.ToString(); // 业务回调按 peer 归档明文
            if (key == m_state.m_throwPlaintextPeer) {
                throw std::runtime_error("intentional peer-local DTLS callback failure");
            }
            m_state.m_plaintextByPeer[key] = std::string(input.AsStringView());
            ++m_state.m_plaintextCallbacks[key];
            input.RetrieveAll();
            m_state.m_totalPlaintextCallbacks.fetch_add(1, std::memory_order_release);
        }

        void OnDtlsHandshakeDone(
            const LikesProgram::Net::Address& peer,
            const char*) override {
            ++m_state.m_handshakeCallbacks[peer.ToString()];
        }

        void OnDtlsSessionError(const LikesProgram::Net::Address& peer, int error) override {
            const std::string key = peer.ToString();
            ++m_state.m_sessionErrors[key];
            m_state.m_lastSessionError[key] = error;
        }

        void OnDtlsSessionClosed(const LikesProgram::Net::Address& peer) override {
            ++m_state.m_closedCallbacks[peer.ToString()];
        }

    private:
        DtlsServerRoutingState& m_state; // issuer 线程内的多 peer 观察状态
    };

    struct DatagramObservation {
        std::atomic<std::size_t> m_received{ 0 };      // 已进入 OnDatagram 的数据报数
        std::atomic<std::size_t> m_zeroLength{ 0 };    // 已观察的空数据报数
        std::atomic<std::size_t> m_originalBytes{ 0 }; // 最近数据报的内核原始长度
        std::atomic<bool> m_truncated{ false };        // 最近一次回调的截断标志
        std::atomic<LikesProgram::Net::EventLoop*> m_loop{ nullptr }; // server completion 统计入口
    };

    class DatagramEchoConnection final : public LikesProgram::Net::Connection {
    public:
        // 创建限制为 4 字节的 UDP completion 连接，强制覆盖截断边界。
        DatagramEchoConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            DatagramObservation& observation)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_observation(observation) {
            SetMaxDatagramBytes(4);
            m_observation.m_loop.store(loop, std::memory_order_release);
        }

    protected:
        // 使用显式 peer 原样回送容量内 payload，不依赖最近 peer 缓存。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address& peer,
            std::size_t originalBytes,
            bool truncated) override {
            m_observation.m_received.fetch_add(1, std::memory_order_acq_rel);
            if (originalBytes == 0) {
                m_observation.m_zeroLength.fetch_add(1, std::memory_order_acq_rel);
            }
            m_observation.m_originalBytes.store(originalBytes, std::memory_order_release);
            m_observation.m_truncated.store(truncated, std::memory_order_release);
            SendTo(peer, input.Peek(), input.ReadableBytes());
            input.RetrieveAll();
        }

    private:
        DatagramObservation& m_observation; // 测试线程读取的 completion 元数据
    };

    class DatagramSequenceConnection final : public LikesProgram::Net::Connection {
    public:
        // 捕获预装载数据报序号，并在完整批次交付后关闭测试循环。
        DatagramSequenceConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::vector<std::uint32_t>& sequences,
            std::atomic<std::size_t>& received,
            std::atomic<bool>& metadataValid)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_loop(loop),
            m_sequences(sequences),
            m_received(received),
            m_metadataValid(metadataValid) {
            SetMaxDatagramBytes(sizeof(std::uint32_t));
        }

    protected:
        // 每个 CQE 必须对应一个完整序号数据报，并保留发送方地址。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address& peer,
            std::size_t originalBytes,
            bool truncated) override {
            if (!peer.IsValid() || originalBytes != sizeof(std::uint32_t)
                || truncated || input.ReadableBytes() != sizeof(std::uint32_t)) {
                m_metadataValid.store(false, std::memory_order_release);
            }

            std::uint32_t sequence = 0; // 当前数据报携带的单调序号
            if (input.ReadableBytes() == sizeof(sequence)) {
                std::memcpy(&sequence, input.Peek(), sizeof(sequence));
                m_sequences.push_back(sequence);
            }
            input.RetrieveAll();

            const std::size_t count = m_received.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (count == 64) {
                ForceClose(); // terminal cancel 与连接清理由既有关闭状态机完成
                if (m_loop != nullptr) m_loop->Shutdown();
            }
        }

    private:
        LikesProgram::Net::EventLoop* m_loop = nullptr; // 完整批次交付后的停止入口
        std::vector<std::uint32_t>& m_sequences; // issuer 线程独占写入的接收顺序
        std::atomic<std::size_t>& m_received; // 测试线程等待的发布计数
        std::atomic<bool>& m_metadataValid; // peer、长度与截断契约汇总
    };

    class DatagramBatchSendConnection final : public LikesProgram::Net::Connection {
    public:
        // 创建 connected UDP 发送端，并发布写队列最终排空状态。
        DatagramBatchSendConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::atomic<bool>& writeComplete,
            std::atomic<bool>* queued = nullptr)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_writeComplete(writeComplete),
            m_queued(queued) {
        }

    protected:
        // 同一 issuer 回调连续排队，给 linked SQE fast path 留出稳定 FIFO 批次。
        void OnConnected() override {
            for (std::uint32_t sequence = 0; sequence < 64; ++sequence) {
                LikesProgram::Net::Buffer payload; // 每个节点独占一个完整数据报所有权
                payload.Append(&sequence, sizeof(sequence));
                Send(std::move(payload));
            }
            if (m_queued != nullptr) m_queued->store(true, std::memory_order_release);
        }

        // 最后一个发送 CQE 回收后发布排空结果。
        void OnWriteComplete() override {
            m_writeComplete.store(true, std::memory_order_release);
        }

    private:
        std::atomic<bool>& m_writeComplete; // 测试线程等待的发送完成屏障
        std::atomic<bool>* m_queued = nullptr; // 可选的全部业务写已登记屏障
    };

    constexpr std::size_t kDatagramForceCloseCount = 64; // 同时覆盖活动 linked 批次与未提交尾队列

    struct DatagramForceCloseObservation {
        std::array<std::array<std::uint8_t, sizeof(std::uint32_t)>,
            kDatagramForceCloseCount> m_storage{}; // 每个 lease 独占且跨 CQE 稳定的 payload
        std::array<std::atomic<unsigned int>,
            kDatagramForceCloseCount> m_releaseCounts{}; // 每个已提交/未提交节点的归还次数
        std::atomic<bool> m_forceCloseRequested{ false }; // Low 回调已安排强制关闭
        std::atomic<bool> m_closed{ false }; // OnClosed 已完成统一关闭通知
        std::atomic<bool> m_shutdownPendingAtClose{ false }; // 关闭时仍有 read/send terminal CQE
        std::atomic<std::size_t> m_pendingAtForceClose{ 0 }; // 关闭请求时尚未完成的数据报数
        std::atomic<std::size_t> m_batchPeakAtForceClose{ 0 }; // 关闭请求前已观测 linked 峰值
        std::atomic<std::uint64_t> m_readSubmissionsAtForceClose{ 0 }; // 证明接收 operation 已提交
        std::atomic<std::size_t> m_callbacksAfterClose{ 0 }; // OnClosed 后不得再进入业务回调
    };

    // 每个发送 lease 只能由成功 CQE、ForceClose 尾队列或 cancel CQE 之一归还一次。
    void CountDatagramForceCloseRelease(void* owner, std::uint32_t token) noexcept {
        auto* observation = static_cast<DatagramForceCloseObservation*>(owner); // 测试作用域覆盖 Poller
        if (observation == nullptr || token >= observation->m_releaseCounts.size()) return;
        observation->m_releaseCounts[token].fetch_add(1, std::memory_order_acq_rel);
    }

    class DatagramForceCloseConnection final : public LikesProgram::Net::Connection {
    public:
        // 240/220 字节水位在首个 16 项 linked 批次中途触发强制关闭。
        DatagramForceCloseConnection(
            LikesProgram::Net::SocketType fd,
            InspectableCompletionEventLoop* loop,
            DatagramForceCloseObservation& observation)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_loop(loop),
            m_observation(observation) {
            SetWriteWatermark(240, 220);
        }

    protected:
        // 预分配 lease-backed 数据报，使关闭同时覆盖已提交批次与未提交 FIFO 所有权。
        void OnConnected() override {
            for (std::uint32_t token = 0; token < kDatagramForceCloseCount; ++token) {
                std::memcpy(
                    m_observation.m_storage[token].data(),
                    &token,
                    sizeof(token));
                auto lease = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
                    m_observation.m_storage[token].data(),
                    sizeof(token),
                    &m_observation,
                    token,
                    &CountDatagramForceCloseRelease);
                LikesProgram::Net::Buffer payload; // Poller 节点直接持有原 lease 所有权
                payload.Append(std::move(lease));
                Send(std::move(payload));
            }
        }

        // linked 批次尚有 CQE 且尾队列未提交时安排 ForceClose。
        void OnWriteLowWatermark(std::size_t) override {
            if (m_observation.m_forceCloseRequested.exchange(
                    true,
                    std::memory_order_acq_rel)) return;
            const LikesProgram::Net::CompletionStats stats =
                m_loop->GetCompletionStats(); // issuer 线程读取同一 Poller 快照
            m_observation.m_pendingAtForceClose.store(
                stats.pendingDatagramSends,
                std::memory_order_release);
            m_observation.m_batchPeakAtForceClose.store(
                stats.maximumDatagramSendBatch,
                std::memory_order_release);
            m_observation.m_readSubmissionsAtForceClose.store(
                m_loop->CompletionReadSubmissionCount(),
                std::memory_order_release);
            ForceClose(); // 任务在当前 CQ 批次后取消活动 read 与 linked send
        }

        // 强制关闭后任何迟到数据报都属于错误业务回调。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address&,
            std::size_t,
            bool) override {
            RecordCallbackAfterClose();
            input.RetrieveAll();
        }

        // ForceClose 不得把 cancel 后的发送尾部误报为完整排空。
        void OnWriteComplete() override {
            RecordCallbackAfterClose();
        }

        // closing 状态的取消 CQE 不得升级为用户错误回调。
        void OnError(int) override {
            RecordCallbackAfterClose();
        }

        // StopConnection 已发布 cancel 时必须仍可观察待回收 terminal CQE。
        void OnClosed() override {
            m_observation.m_shutdownPendingAtClose.store(
                m_loop->HasPendingShutdownCompletions(),
                std::memory_order_release);
            m_observation.m_closed.store(true, std::memory_order_release);
            m_loop->Shutdown(); // EventLoop 退出阶段继续排空 read/send cancel completion
        }

    private:
        // OnClosed 发布后统计任何不应再到达的业务层通知。
        void RecordCallbackAfterClose() noexcept {
            if (m_observation.m_closed.load(std::memory_order_acquire)) {
                m_observation.m_callbacksAfterClose.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        InspectableCompletionEventLoop* m_loop = nullptr; // 真实 Poller 关闭统计入口
        DatagramForceCloseObservation& m_observation; // 测试线程读取的所有权与回调状态
    };

    class DatagramPoolExhaustionConnection final : public LikesProgram::Net::Connection {
    public:
        // 保留每个 UDP lease，直到测试线程显式归还一个 token。
        DatagramPoolExhaustionConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::vector<LikesProgram::Net::Buffer>& retained,
            std::atomic<std::size_t>& received)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_retained(retained),
            m_received(received) {
            SetMaxDatagramBytes(sizeof(std::uint32_t));
        }

    protected:
        // pool 耗尽前不消费 lease；归还一个 token 后额外数据报必须继续交付。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address&,
            std::size_t,
            bool) override {
            m_retained.push_back(std::move(input)); // reserve 后不会在回调热路径扩容
            (void)m_received.fetch_add(1, std::memory_order_acq_rel);
        }

    private:
        std::vector<LikesProgram::Net::Buffer>& m_retained; // 按 token 数保留的 provided-buffer lease
        std::atomic<std::size_t>& m_received; // 测试线程等待 pool 耗尽与恢复
    };

    class DatagramLeaseLifecycleConnection final : public LikesProgram::Net::Connection {
    public:
        // 将接收 lease 移出回调，用于覆盖暂停、跨线程释放和 Poller 后延寿。
        DatagramLeaseLifecycleConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::Buffer& retained,
            std::atomic<std::size_t>& received)
            : Connection(fd, loop, LikesProgram::Net::TransportKind::Udp),
            m_loop(loop),
            m_retained(retained),
            m_received(received) {
            SetMaxDatagramBytes(32);
        }

    protected:
        // 前两条暂停读以验证完整归还，第三条关闭并跨 Poller 延长 pool 生命周期。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address&,
            std::size_t,
            bool) override {
            m_retained = std::move(input); // Buffer PImpl 移动后 lease 可跨回调存活
            const std::size_t count = m_received.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (count == 1) {
                PauseReading(); // cancel 当前 multishot，恢复前不得继续业务回调
            }
            else if (count == 2) {
                PauseReading(); // 第二条归还并收敛全部 token 后再恢复第三条
            }
            else if (count == 3) {
                ForceClose();
                if (m_loop != nullptr) m_loop->Shutdown();
            }
        }

    private:
        LikesProgram::Net::EventLoop* m_loop = nullptr; // 第三条数据报后的安全停止入口
        LikesProgram::Net::Buffer& m_retained; // 测试作用域持有的跨回调 payload
        std::atomic<std::size_t>& m_received; // 暂停/恢复回调计数
    };

    class DatagramCaptureConnection final : public LikesProgram::Net::Connection {
    public:
        using Connection::Connection;

        bool m_called = false;                         // 是否进入过 peer-aware callback
        LikesProgram::Net::Address m_peer;            // Poller 交付的发送方地址
        std::size_t m_originalBytes = 0;               // 内核报告的原始数据报长度
        bool m_truncated = false;                      // 容量不足时的截断标志
        std::string m_payload;                         // callback 范围内复制的业务 payload

    protected:
        // 捕获 fake Poller 交付的完整数据报元数据。
        void OnDatagram(
            LikesProgram::Net::Buffer& input,
            const LikesProgram::Net::Address& peer,
            std::size_t originalBytes,
            bool truncated) override {
            m_called = true;
            m_peer = peer;
            m_originalBytes = originalBytes;
            m_truncated = truncated;
            m_payload.assign(input.AsStringView());
            input.RetrieveAll();
        }
    };

    class CaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 保存测试断言需要共享的接收状态。
        CaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::atomic<bool>& received,
            std::string& payload,
            LikesProgram::Net::TransportKind kind = LikesProgram::Net::TransportKind::Tcp)
            : Connection(fd, loop, kind),
            m_received(received),
            m_payload(payload) {
        }

    protected:
        // 收到回显后记录文本并关闭连接。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            m_payload.assign(in.AsStringView());
            in.RetrieveAll();
            m_received.store(true, std::memory_order_release);
            ForceClose();
        }

    private:
        std::atomic<bool>& m_received; // 测试线程观察的接收完成标记
        std::string& m_payload;        // 测试线程读取的回显内容
    };

    class OwningReadConnection final : public LikesProgram::Net::Connection {
    public:
        OwningReadConnection()
            : Connection(LikesProgram::Net::kInvalidSocket, nullptr) {
        }

        const std::string& Payload() const noexcept { return m_payload; }

    protected:
        // 捕获 epoll 交付的 owning Buffer，并模拟业务同步消费输入。
        void OnMessage(LikesProgram::Net::Buffer& input) override {
            m_payload.assign(input.AsStringView());
            input.RetrieveAll();
        }

    private:
        std::string m_payload; // 保存 owning completion 交付的完整内容
    };

    class BundleCaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 累计一次预装大 payload 的全部 completion 数据，验证跨 buffer 顺序。
        BundleCaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::size_t expectedBytes,
            std::string& payload,
            std::atomic<bool>& completed)
            : Connection(fd, loop),
            m_expectedBytes(expectedBytes),
            m_payload(payload),
            m_completed(completed) {
        }

    protected:
        // 按 Poller 交付顺序追加每个 lease，并立即归还 provided buffer。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            const std::size_t readableBytes = in.ReadableBytes(); // 当前 bundle segment 字节数
            m_payload.append(
                reinterpret_cast<const char*>(in.Peek()),
                readableBytes);
            in.RetrieveAll();
            if (m_payload.size() >= m_expectedBytes) {
                m_completed.store(true, std::memory_order_release);
            }
        }

    private:
        std::size_t m_expectedBytes = 0;        // 完成标志对应的总 payload 字节数
        std::string& m_payload;                 // issuer 线程按 completion 顺序写入
        std::atomic<bool>& m_completed;         // release/acquire 发布完整 payload
    };

    class RetainBufferConnection final : public LikesProgram::Net::Connection {
    public:
        // 将 completion 输入 Buffer 移出回调，模拟上层跨生命周期保留密文或明文段。
        RetainBufferConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::Buffer& retained,
            std::atomic<bool>& received)
            : Connection(fd, loop),
            m_retained(retained),
            m_received(received) {
        }

    protected:
        // 转移 lease 所有权后关闭连接，Buffer 必须独立于 Poller 生命周期继续有效。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            m_retained = std::move(in);
            m_received.store(true, std::memory_order_release);
            ForceClose();
        }

    private:
        LikesProgram::Net::Buffer& m_retained; // 子进程中跨 EventLoop 作用域保留的输入 Buffer
        std::atomic<bool>& m_received;          // 测试线程等待 completion 回调完成
    };

    class GenerationCaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 依次保留切换前后两次 completion，观察各自 generation 的稳定存储。
        GenerationCaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            LikesProgram::Net::Buffer& first,
            LikesProgram::Net::Buffer& second,
            std::atomic<int>& received)
            : Connection(fd, loop),
            m_first(first),
            m_second(second),
            m_received(received) {
        }

    protected:
        // 每条发送都等待前一条完成，单次回调只对应一个 generation。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            const int index = m_received.load(std::memory_order_acquire); // 当前应接管的 completion 序号
            if (index == 0) m_first = std::move(in);
            else m_second = std::move(in);
            m_received.fetch_add(1, std::memory_order_acq_rel);
        }

    private:
        LikesProgram::Net::Buffer& m_first;     // 切换前 generation 的 lease
        LikesProgram::Net::Buffer& m_second;    // 切换后 generation 的 lease
        std::atomic<int>& m_received;            // 已接管 completion 数量
    };

    struct PoolCaptureState {
        std::mutex mutex;                       // 保护 payloads
        std::condition_variable cv;             // 等待池化客户端收到回显
        std::vector<std::string> payloads;      // 按收到顺序记录回显内容
    };

    class PoolCaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 连接池客户端连接不主动关闭，方便测试空闲归还后的复用语义。
        PoolCaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::shared_ptr<PoolCaptureState> state)
            : Connection(fd, loop),
            m_state(std::move(state)) {
        }

    protected:
        // 记录回显但保持连接打开，租约释放时由连接池决定复用或丢弃。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            std::string payload(in.AsStringView());
            in.RetrieveAll();

            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                m_state->payloads.push_back(std::move(payload));
            }
            m_state->cv.notify_all();
        }

    private:
        std::shared_ptr<PoolCaptureState> m_state; // 测试线程与连接回调共享的接收状态
    };

    class PoolShutdownOnConnectedConnection final : public LikesProgram::Net::Connection {
    public:
        // 保存池关闭回调，验证 worker 自线程关闭不会释放仍在栈上的 EventLoop。
        PoolShutdownOnConnectedConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::function<void()> shutdownCallback)
            : Connection(fd, loop),
            m_shutdownCallback(std::move(shutdownCallback)) {
        }

    protected:
        // 首次连接回调内同步关闭所属连接池。
        void OnConnected() override {
            if (m_shutdownCallback) m_shutdownCallback();
        }

    private:
        std::function<void()> m_shutdownCallback; // 指向测试作用域内仍存活的 ConnectionPool
    };

    bool WaitPoolPayload(
        const std::shared_ptr<PoolCaptureState>& state,
        std::size_t index,
        const std::string& expected) {
        std::unique_lock<std::mutex> lock(state->mutex); // 等待指定序号的回显到达
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const bool ready = state->cv.wait_until(lock, deadline, [&]() {
            return state->payloads.size() > index;
        });
        return ready && state->payloads[index] == expected;
    }

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    class ImmediateUpgradeConnection final : public LikesProgram::Net::Connection {
    public:
        // 用于验证连接建立后可由虚事件立即切入安全层握手。
        ImmediateUpgradeConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::unique_ptr<LikesProgram::Net::Transport> transport,
            std::atomic<int>& handshakes)
            : Connection(fd, loop, std::move(transport)),
            m_handshakes(handshakes) {
        }

    protected:
        // 安全层资源准备完成后立即请求升级，模拟连接后立刻 TLS/SSL。
        void OnSecureLayerReady() override {
            UpgradeCommunication();
        }

        // 握手完成后记录次数，证明业务读写前已经进入安全态。
        void OnHandshakeDone() override {
            m_handshakes.fetch_add(1, std::memory_order_acq_rel);
        }

        // 收到数据后回显，验证握手完成后仍能保留通信能力。
        void OnMessage(LikesProgram::Net::Buffer& in) override {
            Send(in.Peek(), in.ReadableBytes());
            in.RetrieveAll();
        }

    private:
        std::atomic<int>& m_handshakes; // 测试线程观察的握手完成次数
    };

    template <typename BaseTransport>
    class UserSecureTransportBase final : public BaseTransport {
    public:
        // 测试用安全层扩展子类不链接任何 TLS 库，只验证继承契约可用。
        explicit UserSecureTransportBase(LikesProgram::Net::SocketType fd)
            : BaseTransport(fd) {
        }

        // 测试实现记录安全层初始化入口是否被调用。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            m_initialized = true;
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现记录通信升级入口是否被调用。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            m_upgraded = true;
            this->BeginSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现将一次握手推进映射为状态切换，模拟 TLS/SSL 完成。
        LikesProgram::Net::IoResult Handshake() override {
            ++m_handshakeCount;
            this->CompleteSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试实现默认关注读事件。
        bool RemainWantRead() const override {
            return true;
        }

        // 测试实现不需要写事件推进握手。
        bool RemainWantWrite() const override {
            return false;
        }

        bool HandshakeCompleted() const noexcept {
            return m_handshakeCount > 0;
        }

        bool Initialized() const noexcept {
            return m_initialized;
        }

        bool Upgraded() const noexcept {
            return m_upgraded;
        }

        bool SocketWriteUsed() const noexcept {
            return m_socketWriteCount > 0;
        }

        bool SecureWriteUsed() const noexcept {
            return m_secureWriteCount > 0;
        }

    protected:
        // 测试实现不执行真实普通 socket 读取，避免依赖无效 socket。
        LikesProgram::Net::IoResult ReadSocketSome(LikesProgram::Net::Buffer&) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }

        // 测试实现假装普通 socket 写入全部字节。
        LikesProgram::Net::IoResult WriteSocketSome(const std::uint8_t*, std::size_t len) override {
            ++m_socketWriteCount;
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }

        // 测试实现假装安全层写入全部字节。
        LikesProgram::Net::IoResult WriteSecureSome(const std::uint8_t*, std::size_t len) override {
            ++m_secureWriteCount;
            return LikesProgram::Net::IoResult{
                LikesProgram::Net::IoStatus::Ok,
                static_cast<std::int64_t>(len),
                0
            };
        }

        // 测试实现无需关闭写方向。
        void ShutdownSocketWrite() override {
        }

        // 测试实现只分离无效 socket，不触碰第三方资源。
        void CloseSocket() override {
            (void)this->DetachFd();
        }

    private:
        bool m_initialized = false; // 记录初始化钩子的调用状态
        bool m_upgraded = false;    // 记录升级钩子的调用状态
        int m_handshakeCount = 0;   // 记录握手推进次数
        int m_socketWriteCount = 0; // 记录普通 socket 写钩子次数
        int m_secureWriteCount = 0; // 记录安全层写钩子次数
    };

    using UserSecureTcpTransport = UserSecureTransportBase<LikesProgram::Net::TcpTransport>;
    using UserSecureUdpTransport = UserSecureTransportBase<LikesProgram::Net::UdpTransport>;

    class BlockingTcpTransport final : public LikesProgram::Net::TcpTransport {
    public:
        // 慢连接测试用 transport 永远写阻塞，迫使 Connection 进入发送队列背压路径。
        explicit BlockingTcpTransport(LikesProgram::Net::SocketType fd)
            : TcpTransport(fd) {
        }

    protected:
        // 测试不需要真实读入，保持非阻塞暂不可读语义。
        LikesProgram::Net::IoResult ReadSocketSome(LikesProgram::Net::Buffer&) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }

        // 永远返回 WouldBlock，验证发送数据会留在 Connection 写队列。
        LikesProgram::Net::IoResult WriteSocketSome(const std::uint8_t*, std::size_t) override {
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::WouldBlock, 0, 0 };
        }

        // 测试对象没有真实 socket 写端，不执行系统调用。
        void ShutdownSocketWrite() override {
        }

        // 分离无效 socket，避免测试析构触碰平台 close。
        void CloseSocket() override {
            (void)this->DetachFd();
        }
    };

    class ReuseFdOnCloseTcpTransport final : public LikesProgram::Net::TcpTransport {
    public:
        // 关闭钩子模拟内核立即复用刚释放的 fd。
        ReuseFdOnCloseTcpTransport(
            LikesProgram::Net::SocketType fd,
            std::function<void(LikesProgram::Net::SocketType)> onClose)
            : TcpTransport(fd, LikesProgram::Net::TcpUpgradeMode::Disabled),
            m_onClose(std::move(onClose)) {
        }

    protected:
        // 测试不触碰真实 socket，只在分离后同步创建同 fd 新连接。
        void CloseSocket() override {
            const LikesProgram::Net::SocketType fd = this->DetachFd(); // 模拟已归还给内核的 fd
            if (m_onClose) m_onClose(fd);
        }

    private:
        std::function<void(LikesProgram::Net::SocketType)> m_onClose; // fd 复用注入点
    };

    class BackpressureCaptureConnection final : public LikesProgram::Net::Connection {
    public:
        // 捕获背压回调次数和队列长度，供测试线程断言。
        BackpressureCaptureConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            std::unique_ptr<LikesProgram::Net::Transport> transport,
            std::atomic<int>& highWatermarkCount,
            std::atomic<int>& overflowCount,
            std::atomic<std::size_t>& pendingBytes)
            : Connection(fd, loop, std::move(transport)),
            m_highWatermarkCount(highWatermarkCount),
            m_overflowCount(overflowCount),
            m_pendingBytes(pendingBytes) {
        }

    protected:
        // 高水位回调中暂停读，验证用户能在底层背压信号上阻断上游输入。
        void OnWriteHighWatermark(std::size_t pendingBytes) override {
            m_pendingBytes.store(pendingBytes, std::memory_order_release);
            m_highWatermarkCount.fetch_add(1, std::memory_order_acq_rel);
            PauseReading();
        }

        // 硬上限溢出表示慢连接需要被底层保护性关闭。
        void OnWriteQueueOverflow(std::size_t pendingBytes) override {
            m_pendingBytes.store(pendingBytes, std::memory_order_release);
            m_overflowCount.fetch_add(1, std::memory_order_acq_rel);
        }

    private:
        std::atomic<int>& m_highWatermarkCount;       // 高水位触发次数
        std::atomic<int>& m_overflowCount;            // 硬上限溢出次数
        std::atomic<std::size_t>& m_pendingBytes;     // 最近一次回调看到的待发送字节
    };

    class StartTlsLikeTcpTransport final : public LikesProgram::Net::TcpTransport {
    public:
        // 共享配置模拟 SSL_CTX/证书链等昂贵资源，连接级初始化只持有引用。
        struct SharedSecureContext {
            std::atomic<int> loadCount{ 0 }; // 共享资源加载次数，测试中必须保持一次
        };

        StartTlsLikeTcpTransport(
            LikesProgram::Net::SocketType fd,
            std::shared_ptr<SharedSecureContext> context,
            LikesProgram::Net::TcpUpgradeMode upgradeMode = LikesProgram::Net::TcpUpgradeMode::Manual,
            bool autoImmediate = false)
            : TcpTransport(fd, upgradeMode),
            m_context(std::move(context)),
            m_autoImmediate(autoImmediate) {
        }

        // 初始化连接级安全会话，并复用 TcpTransport 的升级策略。
        LikesProgram::Net::IoResult InitializeSecureLayer() override {
            ++m_sessionInitCount;
            return ApplyConfiguredUpgradeMode();
        }

        // 显式升级才进入握手态，用于 STARTTLS 或连接后立即 TLS。
        LikesProgram::Net::IoResult UpgradeCommunication() override {
            ++m_upgradeCount;
            BeginSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        // 测试握手一步完成，真实 TLS 可在这里处理 WANT_READ/WANT_WRITE。
        LikesProgram::Net::IoResult Handshake() override {
            ++m_handshakeCount;
            CompleteSecureHandshake();
            return LikesProgram::Net::IoResult{ LikesProgram::Net::IoStatus::Ok, 0, 0 };
        }

        int SessionInitCount() const noexcept {
            return m_sessionInitCount;
        }

        int UpgradeCount() const noexcept {
            return m_upgradeCount;
        }

        int HandshakeCount() const noexcept {
            return m_handshakeCount;
        }

        std::shared_ptr<SharedSecureContext> SharedContext() const {
            return m_context;
        }

        bool DetectsAutoUpgradePacket(const std::uint8_t* data, std::size_t len) const noexcept {
            return ShouldAutoUpgradeFromPeekedBytes(data, len);
        }

    protected:
        // Auto 客户端可由端口/配置判断是否等价于隐式 TLS。
        bool ShouldAutoUpgradeImmediately() const noexcept override {
            return m_autoImmediate;
        }

    private:
        std::shared_ptr<SharedSecureContext> m_context; // 模拟外部共享 TLS 上下文
        int m_sessionInitCount = 0;                     // 每连接会话初始化次数
        int m_upgradeCount = 0;                         // 显式升级次数
        int m_handshakeCount = 0;                       // 握手推进次数
        bool m_autoImmediate = false;                   // Auto 初始化期是否立即升级
    };

#endif

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    struct SharedSecureFactoryState {
        std::atomic<int> sharedInitCount{ 0 }; // 共享资源初始化次数
        std::atomic<int> connectionCreateCount{ 0 }; // 连接创建次数
    };
#endif

    class TestTlsEngine final : public LikesProgram::Net::TlsEngine {
    public:
        // 生成固定握手密文，验证 action 可以同时表达输出与继续收包。
        LikesProgram::Net::TlsResult StartHandshake(
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            LikesProgram::Net::Buffer handshake; // 模拟 memory BIO 产生的握手记录
            handshake.Append("client-hello", 12);
            ciphertextOutput.Append(std::move(handshake));
            m_state = LikesProgram::Net::TlsState::Handshaking;
            return {
                LikesProgram::Net::TlsAction::CiphertextReady
                    | LikesProgram::Net::TlsAction::NeedCiphertext,
                0
            };
        }

        // 消费全部测试密文并产生固定明文，验证输入所有权仍由调用方 chain 管理。
        LikesProgram::Net::TlsResult ConsumeCiphertext(
            LikesProgram::Net::BufferChain& ciphertextInput,
            LikesProgram::Net::BufferChain& plaintextOutput,
            LikesProgram::Net::BufferChain&) override {
            const std::size_t consumed = ciphertextInput.ReadableBytes(); // 本轮消费的 completion 密文字节
            ciphertextInput.Consume(consumed);

            LikesProgram::Net::Buffer plaintext; // 模拟 TLS 解密后的应用数据
            plaintext.Append("plaintext", 9);
            plaintextOutput.Append(std::move(plaintext));
            m_state = LikesProgram::Net::TlsState::Active;
            return { LikesProgram::Net::TlsAction::PlaintextReady, 0 };
        }

        // 将多段明文复制为测试密文，真实适配器会改为 memory/custom BIO。
        LikesProgram::Net::TlsResult ConsumePlaintext(
            LikesProgram::Net::BufferChain& plaintextInput,
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            LikesProgram::Net::Buffer ciphertext; // 汇总测试输入，验证跨段遍历契约
            const std::size_t segmentCount = plaintextInput.SegmentCount(); // 当前明文段数
            for (std::size_t index = 0; index < segmentCount; ++index) {
                const LikesProgram::Net::BufferSlice segment = plaintextInput.Segment(index); // 当前只读段
                ciphertext.Append(segment.Data(), segment.Size());
            }
            plaintextInput.Consume(plaintextInput.ReadableBytes());
            ciphertextOutput.Append(std::move(ciphertext));
            return { LikesProgram::Net::TlsAction::CiphertextReady, 0 };
        }

        // 生成固定关闭记录，并要求 Net 在发送完成后关闭 socket transport。
        LikesProgram::Net::TlsResult Shutdown(
            LikesProgram::Net::BufferChain& ciphertextOutput) override {
            LikesProgram::Net::Buffer closeNotify; // 模拟 TLS close_notify record
            closeNotify.Append("close", 5);
            ciphertextOutput.Append(std::move(closeNotify));
            m_state = LikesProgram::Net::TlsState::Closed;
            return {
                LikesProgram::Net::TlsAction::CiphertextReady
                    | LikesProgram::Net::TlsAction::CloseTransport,
                0
            };
        }

        // 返回当前测试 Engine 状态。
        LikesProgram::Net::TlsState State() const noexcept override {
            return m_state;
        }

        // 返回固定 ALPN，验证协议文本不依赖第三方库类型。
        const char* NegotiatedProtocol() const noexcept override {
            return m_state == LikesProgram::Net::TlsState::Active ? "h2" : "";
        }

    private:
        LikesProgram::Net::TlsState m_state = LikesProgram::Net::TlsState::Handshaking; // 当前会话状态
    };

    struct TlsCompletionState {
        std::atomic<bool> m_handshakeDone{ false }; // OnHandshakeDone 是否已由 Engine Active 状态触发
        std::atomic<bool> m_closed{ false };        // 连接是否已完成统一关闭流程
        std::atomic<int> m_error{ 0 };              // 最近一次连接错误码
        std::atomic<int> m_highWatermarkCount{ 0 }; // TLS 密文写链越过高水位次数
        std::atomic<int> m_lowWatermarkCount{ 0 };  // TLS 密文写链排空到低水位次数
        std::mutex m_mutex;                         // 保护跨 loop 线程的明文快照
        std::string m_plaintext;                    // OnMessageChain 收到的解密明文
    };

    class TlsCompletionConnection final : public LikesProgram::Net::Connection {
    public:
        // 配置测试 Engine Factory，连接启动后立即升级为直接 TLS。
        TlsCompletionConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop,
            TlsCompletionState& state,
            const LikesProgram::Net::TlsEngineFactory& factory,
            bool immediate = true)
            : Connection(fd, loop),
              m_state(state),
              m_immediate(immediate) {
            SetTlsEngineFactory(factory);
        }

        // 显式启动 STARTTLS，测试代码不需要访问 Engine 或 socket。
        void StartTls() {
            UpgradeCommunication();
        }

    protected:
        // 直接 TLS 在 Poller completion I/O 建立后立即启动握手。
        void OnSecureLayerReady() override {
            if (m_immediate) UpgradeCommunication();
        }

        // Engine 首次进入 Active 时记录握手完成事件。
        void OnHandshakeDone() override {
            m_state.m_handshakeDone.store(true, std::memory_order_release);
        }

        // 记录 completion-native 握手超时等连接错误。
        void OnError(int error) override {
            m_state.m_error.store(error, std::memory_order_release);
        }

        // 记录统一关闭完成，供测试线程等待定时 CQE 生效。
        void OnClosed() override {
            m_state.m_closed.store(true, std::memory_order_release);
        }

        // TLS 背压以 Engine 产出的密文字节为准，复用普通 completion 写队列水位。
        void OnWriteHighWatermark(std::size_t) override {
            m_state.m_highWatermarkCount.fetch_add(1, std::memory_order_acq_rel);
        }

        // 密文 CQE 消费后复用同一低水位解除通知。
        void OnWriteLowWatermark(std::size_t) override {
            m_state.m_lowWatermarkCount.fetch_add(1, std::memory_order_acq_rel);
        }

        // 直接消费多段解密明文，不回退到连续 Buffer 适配路径。
        void OnMessageChain(LikesProgram::Net::BufferChain& in) override {
            std::lock_guard<std::mutex> lock(m_state.m_mutex); // loop 回调与测试线程同步明文快照
            const std::size_t segmentCount = in.SegmentCount(); // Engine 本轮输出段数
            for (std::size_t index = 0; index < segmentCount; ++index) {
                const LikesProgram::Net::BufferSlice segment = in.Segment(index); // 当前明文段
                m_state.m_plaintext.append(
                    reinterpret_cast<const char*>(segment.Data()),
                    segment.Size());
            }
            in.Consume(in.ReadableBytes());
        }

    private:
        TlsCompletionState& m_state; // 测试线程共享的握手与明文状态
        bool m_immediate = true;     // true 为直接 TLS，false 为显式 STARTTLS
    };

    void TestTlsEngineContractAndFactory() {
        std::atomic<int> sharedInitCount{ 0 }; // 复制工厂共享的一次性初始化计数
        LikesProgram::Net::TlsEngineFactory factory(
            []() {
                return std::make_unique<TestTlsEngine>();
            },
            [&sharedInitCount]() {
                sharedInitCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        LikesProgram::Net::TlsEngineFactory copiedFactory(factory); // 复制后仍共享初始化状态

        Require(factory.InitializeSharedResources(), "TLS shared resources should initialize");
        Require(copiedFactory.InitializeSharedResources(), "Copied TLS factory should share initialization");
        Require(sharedInitCount.load(std::memory_order_relaxed) == 1,
            "TLS shared resources should initialize once across factory copies");

        auto engine = copiedFactory.Create(); // 每连接创建独立 Engine
        Require(static_cast<bool>(engine), "TLS factory should create an Engine");

        LikesProgram::Net::BufferChain handshakeOutput; // Engine 产生的握手密文
        const LikesProgram::Net::TlsResult handshake = engine->StartHandshake(handshakeOutput);
        Require(handshake.Succeeded(), "TLS handshake start should succeed");
        Require(handshake.HasAction(LikesProgram::Net::TlsAction::CiphertextReady),
            "TLS handshake should produce ciphertext");
        Require(handshake.HasAction(LikesProgram::Net::TlsAction::NeedCiphertext),
            "TLS handshake should request peer ciphertext");
        Require(handshakeOutput.ReadableBytes() == 12,
            "TLS handshake ciphertext should preserve Engine output");

        LikesProgram::Net::Buffer peerRecord; // 模拟 socket completion 提供的 TLS record
        peerRecord.Append("peer", 4);
        LikesProgram::Net::BufferChain ciphertextInput; // 尚未由 Engine 消费的 socket 密文
        ciphertextInput.Append(std::move(peerRecord));
        LikesProgram::Net::BufferChain plaintextOutput; // Engine 解密后的业务明文
        LikesProgram::Net::BufferChain responseOutput; // 握手推进可能产生的响应密文
        const LikesProgram::Net::TlsResult decrypted = engine->ConsumeCiphertext(
            ciphertextInput,
            plaintextOutput,
            responseOutput);
        Require(decrypted.HasAction(LikesProgram::Net::TlsAction::PlaintextReady),
            "TLS ciphertext consumption should expose plaintext");
        Require(ciphertextInput.Empty(), "TLS Engine should consume input through BufferChain");
        Require(plaintextOutput.Segment(0).AsStringView() == "plaintext",
            "TLS plaintext output should remain in the returned BufferChain");
        Require(std::strcmp(engine->NegotiatedProtocol(), "h2") == 0,
            "TLS Engine should expose negotiated ALPN without third-party types");

        LikesProgram::Net::Buffer applicationData; // 模拟业务层待加密数据
        applicationData.Append("request", 7);
        LikesProgram::Net::BufferChain plaintextInput; // 业务明文所有权链
        plaintextInput.Append(std::move(applicationData));
        LikesProgram::Net::BufferChain ciphertextOutput; // Engine 加密后的 socket 输出链
        const LikesProgram::Net::TlsResult encrypted = engine->ConsumePlaintext(
            plaintextInput,
            ciphertextOutput);
        Require(encrypted.HasAction(LikesProgram::Net::TlsAction::CiphertextReady),
            "TLS plaintext consumption should produce ciphertext");
        Require(plaintextInput.Empty(), "TLS Engine should consume plaintext input explicitly");
        Require(ciphertextOutput.Segment(0).AsStringView() == "request",
            "TLS ciphertext output should preserve test payload");

        LikesProgram::Net::BufferChain closeOutput; // close_notify 等关闭密文
        const LikesProgram::Net::TlsResult closed = engine->Shutdown(closeOutput);
        Require(closed.HasAction(LikesProgram::Net::TlsAction::CloseTransport),
            "TLS shutdown should request transport close after output drain");
        Require(engine->State() == LikesProgram::Net::TlsState::Closed,
            "TLS Engine should expose closed state");
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Net::PackageName();
        const char* packageVersion = LikesProgram::Net::PackageVersion();

        Require(LikesProgram::Net::PackageAvailable(), "Net package should be available");
        Require(std::strcmp(packageName, "LikesProgramNet") == 0, "Net package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Net package version should follow Core version");
    }

    void TestDefaultPollerExposesBackendName() {
        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 默认工厂应返回当前平台 completion 后端

        Require(static_cast<bool>(poller), "Default poller should be created");
        Require(poller->BackendName() != nullptr, "Default poller backend name should not be null");
#if defined(__linux__)
        Require(IsKnownLinuxBackend(poller->BackendName()),
            "Linux Poller should expose one of the two supported completion backends");
        const char* requestedBackend = std::getenv("LIKESPROGRAM_NET_BACKEND");
        const bool forcedEpoll = requestedBackend != nullptr
            && std::strcmp(requestedBackend, "epoll") == 0;
        Require(forcedEpoll
                ? IsEpollBackend(poller->BackendName())
                : IsIoUringBackend(poller->BackendName()),
            "Linux Poller should honor forced epoll and otherwise use available io_uring");
#else
        Require(false, "Only the Linux completion platform is enabled in the current release");
#endif
    }

    int RunBackendContract(const char* requestedBackend) {
        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // focused 模式只验证工厂诊断名
        const char* environmentBackend = std::getenv("LIKESPROGRAM_NET_BACKEND");
        const bool expectsEpoll = (requestedBackend != nullptr
                && std::strcmp(requestedBackend, "epoll") == 0)
            || (requestedBackend == nullptr
                && environmentBackend != nullptr
                && std::strcmp(environmentBackend, "epoll") == 0);
        const char* expectedBackend = expectsEpoll
            ? "epoll-level-completion"
            : "io_uring-multishot-provided-buffer"; // 显式参数或当前矩阵环境决定期望名

        Require(static_cast<bool>(poller), "Backend contract should create a Poller");
        Require(poller->BackendName() != nullptr, "Backend contract name should not be null");
        Require(poller->BackendName()[0] != '\0', "Backend contract name should not be empty");
        Require(std::strcmp(poller->BackendName(), expectedBackend) == 0,
            "Backend contract should expose the requested implementation");
        return 0;
    }

#if defined(__linux__)
    void TestCompletionConnectUsesPollerOperation() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses(); // 获取 port 0 实际绑定端口
        Require(!addresses.empty(), "Connect completion server should expose bound address");

        ConnectCompletionLoop loop;
        std::atomic<bool> completed{ false };              // connect CQE 是否进入 issuer callback
        std::atomic<int> completionError{ EIO };           // callback 返回的 errno 风格结果
        std::atomic<LikesProgram::Net::SocketType> connectedFd{
            LikesProgram::Net::kInvalidSocket };           // 成功后由测试接管并关闭的 socket
        loop.PostTask([&loop, &addresses, &completed, &completionError, &connectedFd]() {
            const auto connectId = loop.StartConnect(
                addresses.front(),
                [&completed, &completionError, &connectedFd](
                    LikesProgram::Net::SocketType fd,
                    int error) {
                    connectedFd.store(fd, std::memory_order_release);
                    completionError.store(error, std::memory_order_release);
                    completed.store(true, std::memory_order_release);
                });
            if (connectId == LikesProgram::Net::Poller::InvalidConnectId) {
                completed.store(true, std::memory_order_release);
            }
        });

        std::thread worker([&loop]() { loop.Start(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2); // 本地回环上界
        while (!completed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const LikesProgram::Net::SocketType fd = connectedFd.load(std::memory_order_acquire);
        if (fd != LikesProgram::Net::kInvalidSocket) (void)::close(fd);
        loop.Shutdown();
        worker.join();
        server.Shutdown();

        Require(completed.load(std::memory_order_acquire),
            "Poller connect completion should finish on loop thread");
        Require(completionError.load(std::memory_order_acquire) == 0,
            "Poller connect completion should report success");
        Require(fd != LikesProgram::Net::kInvalidSocket,
            "Successful connect completion should transfer socket ownership");
        Require(loop.CompletionOperationCount() > 0,
            "Connect should contribute a real Poller completion");
        const auto stats = loop.GetCompletionStats(); // 成功 connect 应留下可诊断提交计数
        Require(stats.connectSubmissions == 1 && stats.pendingConnectOperations == 0,
            "Successful connect completion should retire its diagnostic pending count");
    }

    void TestCompletionConnectCancelSuppressesCallback() {
        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 直接控制首次 Flush 前取消顺序
        Require(static_cast<bool>(poller) && poller->Activate(),
            "Connect cancel Poller should activate");

        bool callbackInvoked = false; // closing 在 SQ 提交前发布，任何原 CQE 都不得进入回调
        const LikesProgram::Net::Address blackhole("192.0.2.1", 9); // RFC 5737 文档网段
        const auto connectId = poller->StartConnect(
            blackhole,
            [&callbackInvoked](LikesProgram::Net::SocketType fd, int) {
                callbackInvoked = true;
                if (fd != LikesProgram::Net::kInvalidSocket) (void)::close(fd);
            });
        Require(connectId != LikesProgram::Net::Poller::InvalidConnectId,
            "Connect cancel should create a pending operation");
        poller->CancelConnect(connectId); // 原 connect 与 cancel 在首次 Flush 同批提交

        std::vector<LikesProgram::Net::Channel*> activeChannels; // connect 不产生兼容 Channel
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        LikesProgram::Net::CompletionStats stats;
        do {
            poller->Poll(10, activeChannels);
            stats = poller->GetCompletionStats();
        } while (stats.pendingConnectOperations != 0
            && std::chrono::steady_clock::now() < deadline);

        Require(!callbackInvoked,
            "Cancelled connect completion must suppress the user callback");
        Require(stats.connectSubmissions == 1,
            "Connect cancel diagnostics should preserve the original submission");
        Require(stats.connectCancelSubmissions >= 1,
            "Connect cancel should submit at least one async cancel SQE");
        Require(stats.pendingConnectOperations == 0,
            "Cancelled connect should retire after its original CQE is reaped");
    }
#endif

    void TestEventLoopExposesCompletionBackendName() {
        LikesProgram::Net::EventLoop loop; // 真实 EventLoop 用于验证运行时后端选择
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        Require(IsKnownLinuxBackend(loop.CompletionBackendName()),
            "Linux EventLoop should expose a supported completion backend");
        const bool epollBackend = IsEpollBackend(loop.CompletionBackendName());
        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 统一反馈快照
        if (epollBackend) {
            Require(loop.CompletionProvidedBufferCount() == 0
                    && !loop.CompletionReceiveBundleEnabled()
                    && stats.providedBufferCount == 0
                    && stats.providedBufferSize == 0
                    && stats.currentAvailableBuffers == 0
                    && !stats.receiveBundleEnabled
                    && stats.datagramProvidedBufferCount == 0
                    && stats.datagramProvidedBufferSize == 0
                    && !stats.datagramMultishotEnabled
                    && stats.datagramActiveBufferLeases == 0,
                "Epoll should keep io_uring-only capability fields at zero");
        }
        else {
            Require(loop.CompletionProvidedBufferCount() > 0,
                "Linux io_uring backend should register provided buffers");
            if (loop.CompletionReceiveBundleEnabled()) {
                Require(loop.CompletionProvidedBufferCount() > 1,
                    "Receive bundle requires a multi-buffer provided ring");
            }
            Require(stats.providedBufferCount == loop.CompletionProvidedBufferCount(),
                "Completion stats should preserve provided-buffer count");
            Require(stats.providedBufferSize > 0,
                "Completion stats should expose a non-zero registered buffer byte size");
            Require(stats.currentAvailableBuffers == stats.providedBufferCount,
                "Fresh completion stats should expose every registered buffer as available");
            Require(stats.receiveBundleEnabled == loop.CompletionReceiveBundleEnabled(),
                "Completion stats should preserve receive bundle capability");
            Require(stats.datagramProvidedBufferCount >= 64,
                "Linux completion backend should expose the optional UDP provided-buffer pool");
            Require(stats.datagramProvidedBufferSize >= 64 * 1024,
                "UDP provided buffers should cover the configured maximum datagram");
        }
        Require(stats.receiveBundleCompletions == 0
                && stats.receiveBundleBuffers == 0
                && stats.maximumReceiveBundleBuffers == 0,
            "Fresh completion stats should not report an unobserved receive bundle CQE");
        Require(stats.datagramMultishotReceiveCompletions == 0
                && stats.datagramReceiveFallbacks == 0
                && stats.datagramSendBatchSubmissions == 0
                && stats.maximumDatagramSendBatch == 0,
            "Fresh UDP batch diagnostics should start empty");
#else
        Require(std::strcmp(loop.CompletionBackendName(), "none") == 0,
            "EventLoop without io_uring should expose no completion backend");
#endif
    }

    void TestEventLoopActivationFailureRunsStartupTasks() {
        auto poller = std::make_unique<OwnershipPoller>(false); // 精确模拟 Poller Activate 失败
        LikesProgram::Net::EventLoop loop(std::move(poller));
        bool startupTaskRan = false; // 启动屏障必须被失败路径显式释放
        loop.PostTask([&startupTaskRan]() { startupTaskRan = true; });

        loop.Start();

        Require(startupTaskRan,
            "EventLoop activation failure should run prequeued startup tasks");
        Require(!loop.IsRunning(),
            "EventLoop activation failure must not report a running loop");
    }

    void TestEventLoopContainsThrowingPendingTask() {
        auto poller = std::make_unique<OwnershipPoller>(); // fake completion 后端让任务顺序完全确定
        LikesProgram::Net::EventLoop loop(std::move(poller));
        bool trailingTaskRan = false; // 抛出任务之后的关闭任务必须继续执行
        loop.PostTask([]() {
            throw std::runtime_error("intentional EventLoop task failure");
        });
        loop.PostTask([&loop, &trailingTaskRan]() {
            trailingTaskRan = true;
            loop.Shutdown();
        });

        loop.Start();

        Require(trailingTaskRan,
            "A throwing EventLoop task must not suppress later issuer tasks");
        Require(!loop.IsRunning(),
            "EventLoop should still reach a clean shutdown after a task exception");
    }

    void TestEventLoopShutdownBeforeStartCannotReviveLoop() {
        auto poller = std::make_unique<OwnershipPoller>(); // Activate 成功也不能覆盖先到的 Shutdown
        LikesProgram::Net::EventLoop loop(std::move(poller));
        bool cancellationTaskRan = false; // 预排队取消通知仍需完成
        loop.PostTask([&cancellationTaskRan]() { cancellationTaskRan = true; });

        loop.Shutdown();
        loop.Start();

        Require(cancellationTaskRan,
            "Shutdown-before-start should drain prequeued cancellation tasks");
        Require(!loop.IsRunning(),
            "Shutdown-before-start must not let Start revive the loop");
    }

    // EventLoop 退出主循环后仍需消费 Poller 报告的取消 completion。
    void TestEventLoopDrainsShutdownCompletions() {
        auto poller = std::make_unique<OwnershipPoller>(); // 用剩余轮询次数模拟 terminal cancel CQE
        OwnershipPoller* pollerAddress = poller.get(); // EventLoop 接管后的观察地址
        LikesProgram::Net::EventLoop loop(std::move(poller));
        loop.PostTask([&loop, pollerAddress]() {
            pollerAddress->ArmShutdownDrain(2); // 主循环本轮结束后仍需要两次 Poll
            loop.Shutdown();
        });

        loop.Start();
        Require(pollerAddress->PollCount() == 3,
            "EventLoop shutdown should poll until cancel completions converge");
        Require(!pollerAddress->HasPendingShutdownCompletions(),
            "EventLoop shutdown should leave no pending cancellation completion");
    }

    void TestCompletionTimeoutFiresAndCancels() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 直接隔离 timer operation 后端语义
        Require(static_cast<bool>(poller) && poller->Activate(),
            "Timeout completion Poller should activate");

        bool fired = false; // 到期回调由当前 issuer 线程在 Poll 内执行
        const LikesProgram::Net::Poller::TimeoutId firedId = poller->ScheduleTimeout(
            std::chrono::milliseconds(10),
            [&fired]() { fired = true; });
        Require(firedId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Timeout completion should return a valid id");
        std::vector<LikesProgram::Net::Channel*> activeChannels; // timer 不应产生伪 Channel
        poller->Poll(100, activeChannels);
        Require(fired, "Timeout completion should invoke its callback after expiry");
        Require(activeChannels.empty(), "Timeout completion should not produce an active Channel");

        bool canceledFired = false; // 取消后原 timeout CQE 只能负责回收状态
        const std::uint64_t completedBeforeCancel = poller->CompletedOperationCount(); // 已完成到期 CQE 数
        const LikesProgram::Net::Poller::TimeoutId canceledId = poller->ScheduleTimeout(
            std::chrono::milliseconds(200),
            [&canceledFired]() { canceledFired = true; });
        Require(canceledId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Cancelable timeout should return a valid id");
        poller->CancelTimeout(canceledId);

        const bool epollBackend = IsEpollBackend(poller->BackendName());
        if (epollBackend) {
            poller->Poll(20, activeChannels); // epoll 同步删除 active timer generation
        }
        else {
            const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (poller->CompletedOperationCount() < completedBeforeCancel + 2
                && std::chrono::steady_clock::now() < cancelDeadline) {
                poller->Poll(20, activeChannels);
            }
        }
        Require(!canceledFired, "Canceled timeout must not invoke its callback");
        Require(epollBackend
                ? poller->CompletedOperationCount() == completedBeforeCancel
                : poller->CompletedOperationCount() >= completedBeforeCancel + 2,
            "Timer cancellation diagnostics should match the selected backend");
#endif
    }

    void TestCompletionTimeoutPreservesBudgetAcrossSignalInterrupts() {
#if defined(__linux__) && defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr); // 隔离 Poll 等待预算与 timer CQE
        Require(static_cast<bool>(poller) && poller->Activate(),
            "Signal-interrupted timeout Poller should activate");

        struct sigaction interruptAction{}; // 不启用 SA_RESTART，确保 wait 能观察 EINTR
        interruptAction.sa_handler = IgnorePollInterruptSignal;
        (void)::sigemptyset(&interruptAction.sa_mask);
        struct sigaction previousAction{}; // 用例退出前恢复进程级 SIGUSR1 行为
        Require(::sigaction(SIGUSR1, &interruptAction, &previousAction) == 0,
            "Signal-interrupted timeout should install its test handler");

        bool fired = false; // timer 必须在同一次 Poll 的原始预算内完成
        const LikesProgram::Net::Poller::TimeoutId timeoutId = poller->ScheduleTimeout(
            std::chrono::milliseconds(100),
            [&fired]() { fired = true; });
        Require(timeoutId != LikesProgram::Net::Poller::InvalidTimeoutId,
            "Signal-interrupted timeout should return a valid id");

        const pthread_t pollingThread = ::pthread_self(); // 信号只定向打断当前 Poll 线程
        std::atomic<bool> pollStarting{ false }; // 避免辅助线程早于 Poll 准备阶段发送
        std::atomic<int> signalFailure{ 0 }; // 保存首个 pthread_kill 错误供主线程断言
        std::thread interrupter([pollingThread, &pollStarting, &signalFailure]() {
            while (!pollStarting.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int attempt = 0; attempt < 12; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const int result = ::pthread_kill(pollingThread, SIGUSR1); // 重复覆盖调度抖动窗口
                if (result != 0) {
                    signalFailure.store(result, std::memory_order_release);
                    break;
                }
            }
        });

        std::vector<LikesProgram::Net::Channel*> activeChannels; // timer 不产生 Channel 事件
        pollStarting.store(true, std::memory_order_release);
        poller->Poll(250, activeChannels);
        interrupter.join();
        const bool restored = ::sigaction(SIGUSR1, &previousAction, nullptr) == 0; // 先恢复再断言

        Require(signalFailure.load(std::memory_order_acquire) == 0,
            "Signal-interrupted timeout should deliver SIGUSR1 to the polling thread");
        Require(restored, "Signal-interrupted timeout should restore the previous signal handler");
        Require(fired, "Timeout Poll should preserve its wait budget across EINTR");
        Require(activeChannels.empty(), "Signal-interrupted timeout should not produce an active Channel");
#endif
    }

    void TestCompletionBackendExecutesPlainTcpIo() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 当前测试直接观察该 loop 的 completion 计数
        int sockets[2] = { -1, -1 }; // socketpair 隔离 built-in plain transport completion 路径
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Completion backend socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "Completion backend sockets should enter nonblocking mode");

        auto connection = std::make_shared<EchoConnection>(sockets[0], &loop);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const char payload[] = "completion"; // 明文 TCP 往返必须由 io_uring read/write 完成
        Require(::send(sockets[1], payload, sizeof(payload) - 1, 0)
            == static_cast<ssize_t>(sizeof(payload) - 1),
            "Completion backend sender should write payload");

        std::array<char, 32> received{}; // 回显接收缓冲
        ssize_t receivedBytes = -1; // 非阻塞轮询得到的最终字节数
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            receivedBytes = ::recv(sockets[1], received.data(), received.size(), 0);
            if (receivedBytes > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        // 对端收到字节早于用户态消费 send CQE，停机前等待 read/write completion 都被分派。
        const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (loop.CompletionOperationCount() < 2
            && std::chrono::steady_clock::now() < completionDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        (void)::close(sockets[1]);
        sockets[1] = -1;
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        loop.Shutdown();
        worker.join();

        Require(receivedBytes == static_cast<ssize_t>(sizeof(payload) - 1),
            "Completion backend should preserve plain TCP round-trip");
        Require(std::string_view(received.data(), static_cast<std::size_t>(receivedBytes)) == payload,
            "Completion backend should preserve payload bytes");
        Require(loop.CompletionOperationCount() >= 2,
            "Completion backend should complete both read and write operations");
        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 真实 CQE 反馈快照
        Require(stats.completedOperations >= 2,
            "Completion stats should count real read and write CQEs");
        Require(stats.completionBatchCount > 0 && stats.completionBatchItems >= 2,
            "Completion stats should record non-empty CQ batches");
        Require(stats.peakCompletionBatch > 0,
            "Completion stats should record a real CQ batch peak");
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // 只有 io_uring 暴露自适应 CQ budget
        if (epollBackend) {
            Require(stats.completionBudget == 0,
                "Epoll completion should not report an io_uring CQ budget");
        }
        else {
            Require(stats.completionBudget >= 64,
                "Adaptive completion budget should preserve its lower safety bound");
        }
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Completion backend should close a connection after peer EOF");
#endif
    }

    void TestTlsEngineRunsOnCompletionDataPath() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 真实 io_uring completion loop
        int sockets[2] = { -1, -1 }; // socketpair 隔离 TLS Engine 数据路径
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "TLS completion socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "TLS completion sockets should enter nonblocking mode");

        LikesProgram::Net::TlsEngineFactory factory([]() {
            return std::make_unique<TestTlsEngine>();
        });
        TlsCompletionState state; // 连接回调与测试线程共享的结果
        auto connection = std::make_shared<TlsCompletionConnection>(
            sockets[0],
            &loop,
            state,
            factory);
        connection->SetWriteWatermark(4, 0); // 握手和业务密文都应越过同一写队列水位
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        std::array<char, 32> handshakeBytes{}; // Engine StartHandshake 产生的首批密文
        std::size_t handshakeSize = 0; // 非阻塞 recv 已累计的握手字节数
        const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (handshakeSize < 12 && std::chrono::steady_clock::now() < handshakeDeadline) {
            const ssize_t received = ::recv(
                sockets[1],
                handshakeBytes.data() + handshakeSize,
                handshakeBytes.size() - handshakeSize,
                0);
            if (received > 0) handshakeSize += static_cast<std::size_t>(received);
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(std::string_view(handshakeBytes.data(), handshakeSize) == "client-hello",
            "TLS Engine handshake ciphertext should flow through Poller QueueWrite(BufferChain)");

        const char peerRecord[] = "peer-record"; // 模拟 peer TLS record completion
        Require(::send(sockets[1], peerRecord, sizeof(peerRecord) - 1, 0)
            == static_cast<ssize_t>(sizeof(peerRecord) - 1),
            "TLS peer should send ciphertext record");

        const auto plaintextDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool plaintextReady = false; // 握手完成且业务收到 Engine 明文
        while (std::chrono::steady_clock::now() < plaintextDeadline) {
            {
                std::lock_guard<std::mutex> lock(state.m_mutex); // 读取 loop 写入的明文快照
                plaintextReady = state.m_plaintext == "plaintext";
            }
            if (plaintextReady && state.m_handshakeDone.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(plaintextReady, "TLS ciphertext completion should produce plaintext chain");
        Require(state.m_handshakeDone.load(std::memory_order_acquire),
            "TLS Engine Active state should trigger OnHandshakeDone once");

        connection->Send("request", 7); // 业务明文必须先经过 Engine 再进入 Poller
        std::array<char, 16> encryptedBytes{}; // 测试 Engine 以同字节模拟加密输出
        ssize_t encryptedSize = -1; // 非阻塞读取的 Engine 密文长度
        const auto encryptedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            encryptedSize = ::recv(sockets[1], encryptedBytes.data(), encryptedBytes.size(), 0);
            if (encryptedSize > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < encryptedDeadline);
        Require(encryptedSize == 7
            && std::string_view(encryptedBytes.data(), static_cast<std::size_t>(encryptedSize)) == "request",
            "TLS plaintext send should produce ciphertext on the same completion write chain");

        const auto watermarkDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while ((state.m_highWatermarkCount.load(std::memory_order_acquire) < 2
            || state.m_lowWatermarkCount.load(std::memory_order_acquire) < 2)
            && std::chrono::steady_clock::now() < watermarkDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(state.m_highWatermarkCount.load(std::memory_order_acquire) >= 2,
            "TLS handshake and application ciphertext should use completion high watermark accounting");
        Require(state.m_lowWatermarkCount.load(std::memory_order_acquire) >= 2,
            "TLS ciphertext CQEs should use completion low watermark accounting");

        connection->Shutdown(); // Engine 先产生 close_notify，写完成后框架再关闭 socket
        std::array<char, 8> closeBytes{}; // 测试 Engine 产生的关闭密文
        ssize_t closeSize = -1; // 非阻塞读取的关闭记录长度
        const auto closeRecordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            closeSize = ::recv(sockets[1], closeBytes.data(), closeBytes.size(), 0);
            if (closeSize > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < closeRecordDeadline);
        Require(closeSize == 5
            && std::string_view(closeBytes.data(), static_cast<std::size_t>(closeSize)) == "close",
            "TLS shutdown should send close_notify before closing the socket transport");

        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)::close(sockets[1]);
        sockets[1] = -1;
        loop.Shutdown();
        worker.join();
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "TLS completion connection should close after close_notify write completion");
#endif
    }

    void TestTlsEngineSupportsManualUpgrade() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // STARTTLS 前后复用同一个 completion loop
        int sockets[2] = { -1, -1 }; // socketpair 隔离延迟升级状态切换
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "STARTTLS socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "STARTTLS sockets should enter nonblocking mode");

        LikesProgram::Net::TlsEngineFactory factory([]() {
            return std::make_unique<TestTlsEngine>();
        });
        TlsCompletionState state; // 当前用例不进入 peer handshake 完成阶段
        auto connection = std::make_shared<TlsCompletionConnection>(
            sockets[0],
            &loop,
            state,
            factory,
            false);
        Require(connection->HasTlsEngineFactory(),
            "STARTTLS connection should retain the configured Engine Factory");
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (loop.CompletionReadSubmissionCount() == 0
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        connection->Send("plain", 5); // 显式升级前仍走明文 completion 写链
        std::array<char, 16> plainBytes{}; // peer 收到的升级前明文
        ssize_t plainSize = -1; // 非阻塞读取的明文长度
        const auto plainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            plainSize = ::recv(sockets[1], plainBytes.data(), plainBytes.size(), 0);
            if (plainSize > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < plainDeadline);
        Require(plainSize == 5
            && std::string_view(plainBytes.data(), static_cast<std::size_t>(plainSize)) == "plain",
            "Configured TLS Factory should not alter plaintext before explicit upgrade");

        connection->StartTls(); // 同一个 Connection 原地切换到 Engine completion 链
        std::array<char, 32> handshakeBytes{}; // 显式升级后产生的握手密文
        ssize_t handshakeSize = -1; // 非阻塞读取的握手密文长度
        const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            handshakeSize = ::recv(sockets[1], handshakeBytes.data(), handshakeBytes.size(), 0);
            if (handshakeSize > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < handshakeDeadline);
        Require(handshakeSize == 12
            && std::string_view(handshakeBytes.data(), static_cast<std::size_t>(handshakeSize)) == "client-hello",
            "Explicit STARTTLS should produce Engine handshake ciphertext");

        (void)::close(sockets[1]);
        sockets[1] = -1;
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "STARTTLS connection should close after peer EOF");
#endif
    }

    void TestTlsHandshakeTimeoutUsesCompletionOperation() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 真实 io_uring timer 与 socket completion 共用同一 loop
        int sockets[2] = { -1, -1 }; // peer 保持连接但不发送握手密文
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "TLS timeout socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "TLS timeout sockets should enter nonblocking mode");

        LikesProgram::Net::TlsEngineFactory factory([]() {
            return std::make_unique<TestTlsEngine>();
        });
        TlsCompletionState state; // 跨线程观察超时错误和关闭完成
        auto connection = std::make_shared<TlsCompletionConnection>(
            sockets[0],
            &loop,
            state,
            factory);
        connection->SetTlsHandshakeTimeout(std::chrono::milliseconds(30));
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!state.m_closed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        (void)::close(sockets[1]);
        sockets[1] = -1;
        loop.Shutdown();
        worker.join();
        Require(state.m_closed.load(std::memory_order_acquire),
            "TLS handshake should close when the timer completion expires");
        Require(state.m_error.load(std::memory_order_acquire) == ETIMEDOUT,
            "TLS handshake timeout should report ETIMEDOUT");
        Require(!state.m_handshakeDone.load(std::memory_order_acquire),
            "TLS timeout must not report a completed handshake");
#endif
    }

    void TestTlsHandshakeCompletionCancelsTimeout() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 验证 read CQE 完成握手后取消同 ring timer
        int sockets[2] = { -1, -1 }; // socketpair 提供确定的握手输入与连接状态
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "TLS timeout cancellation socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "TLS timeout cancellation sockets should enter nonblocking mode");

        LikesProgram::Net::TlsEngineFactory factory([]() {
            return std::make_unique<TestTlsEngine>();
        });
        TlsCompletionState state; // 观察握手完成后是否发生迟到 timeout 关闭
        auto connection = std::make_shared<TlsCompletionConnection>(
            sockets[0],
            &loop,
            state,
            factory);
        connection->SetTlsHandshakeTimeout(std::chrono::milliseconds(200));
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const char peerRecord[] = "peer-record"; // 立即推进 Engine 到 Active，抢在 timer 到期前完成
        Require(::send(sockets[1], peerRecord, sizeof(peerRecord) - 1, 0)
            == static_cast<ssize_t>(sizeof(peerRecord) - 1),
            "TLS timeout cancellation peer should send handshake ciphertext");

        const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!state.m_handshakeDone.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < handshakeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(state.m_handshakeDone.load(std::memory_order_acquire),
            "TLS handshake should finish before its timeout");

        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // 跨过原 timeout 窗口观察迟到 CQE
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Connected,
            "Completed TLS handshake should cancel its timeout operation");
        Require(state.m_error.load(std::memory_order_acquire) == 0,
            "Cancelled TLS handshake timeout should not report an error");

        (void)::close(sockets[1]);
        sockets[1] = -1;
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!state.m_closed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();
        Require(state.m_closed.load(std::memory_order_acquire),
            "TLS timeout cancellation connection should still close on peer EOF");
#endif
    }

    void TestCompletionMultishotReceiveUsesSingleSubmission() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 观察同一连接的 multishot recv 提交次数
        int sockets[2] = { -1, -1 }; // socketpair 提供可重复的双向 TCP stream 语义
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Multishot completion socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "Multishot completion sockets should enter nonblocking mode");

        auto connection = std::make_shared<EchoConnection>(sockets[0], &loop);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        for (const char payload : { 'a', 'b', 'c' }) {
            Require(::send(sockets[1], &payload, 1, 0) == 1,
                "Multishot sender should write one byte");

            char echoed = 0; // 每轮等待回显后再发送下一条，确保产生独立 read CQE
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (::recv(sockets[1], &echoed, 1, 0) != 1
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(echoed == payload, "Multishot completion should preserve each payload");
        }

        Require(loop.CompletionReadSubmissionCount() == 1,
            "One TCP connection should keep one multishot recv submission");
        (void)::close(sockets[1]);
        loop.Shutdown();
        worker.join();
#endif
    }

    void TestCompletionReceiveBundleConsumesMultipleBuffers() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 真实后端决定 feature=true bundle 或稳定 fallback
        int sockets[2] = { -1, -1 }; // receiver 在注册前预装大 payload，避免到达时序拆散首个 recv
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Receive bundle socketpair should open");
        Require(SetTestNonBlocking(sockets[0]),
            "Receive bundle completion receiver should enter nonblocking mode");

        std::thread worker([&loop]() { loop.Start(); });
        const auto activationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!loop.IsRunning() && std::chrono::steady_clock::now() < activationDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(loop.IsRunning(), "Receive bundle EventLoop should activate");

        const LikesProgram::Net::CompletionStats initialStats = loop.GetCompletionStats(); // 运行时池规格
        const bool epollBackend = IsEpollBackend(loop.CompletionBackendName());
        Require(epollBackend || initialStats.providedBufferSize > 0,
            "Receive bundle test requires epoll fallback or registered provided buffers");
        const std::size_t segmentBytes = epollBackend
            ? 64 * 1024
            : initialStats.providedBufferSize;
        const std::size_t payloadBytes = segmentBytes * 3 + 17; // 两种后端都跨越至少四段读取
        std::string expected(payloadBytes, '\0'); // 非文本字节也必须保持原始顺序
        for (std::size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<char>((index * 37 + 11) & 0xff);
        }

        std::size_t sentBytes = 0; // peer 保持 blocking，连接注册前完整预装 socket receive queue
        while (sentBytes < expected.size()) {
            const ssize_t sent = ::send(
                sockets[1],
                expected.data() + sentBytes,
                expected.size() - sentBytes,
                0);
            if (sent < 0 && errno == EINTR) continue;
            Require(sent > 0, "Receive bundle sender should preload the complete payload");
            sentBytes += static_cast<std::size_t>(sent);
        }

        std::string actual; // completed release 后由测试线程读取
        actual.reserve(expected.size());
        std::atomic<bool> completed{ false }; // payload 完整交付标志
        auto connection = std::make_shared<BundleCaptureConnection>(
            sockets[0],
            &loop,
            expected.size(),
            actual,
            completed);
        loop.PostTask([&loop, connection]() {
            loop.AttachConnection(connection);
            connection->Start();
        });

        const auto receiveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!completed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < receiveDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(completed.load(std::memory_order_acquire),
            "Receive bundle completion should deliver the complete payload");
        Require(actual == expected,
            "Receive bundle completion should preserve cross-buffer payload order");

        LikesProgram::Net::CompletionStats finalStats; // 等待 issuer 回填全部已消费 lease token
        const auto returnDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            finalStats = loop.GetCompletionStats();
            if (finalStats.activeBufferLeases == 0
                && finalStats.pendingBufferReturns == 0
                && finalStats.currentAvailableBuffers == finalStats.providedBufferCount) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < returnDeadline);

        Require(finalStats.receivedBytes >= expected.size(),
            "Receive diagnostics should count the complete bundle payload");
        Require(finalStats.activeBufferLeases == 0 && finalStats.pendingBufferReturns == 0,
            "Receive bundle completion should return every consumed lease");
        Require(finalStats.currentAvailableBuffers == finalStats.providedBufferCount,
            "Receive bundle completion should restore the full provided-buffer waterline");
        if (finalStats.receiveBundleEnabled) {
            Require(finalStats.receiveBundleCompletions > 0,
                "Enabled receive bundle should report at least one bundle CQE");
            Require(finalStats.receiveBundleBuffers > finalStats.receiveBundleCompletions,
                "Enabled receive bundle should consume multiple buffers in one or more CQEs");
            Require(finalStats.maximumReceiveBundleBuffers > 1,
                "Enabled receive bundle should prove one CQE consumed multiple buffers");
        }
        else {
            Require(finalStats.receiveBundleCompletions == 0
                    && finalStats.receiveBundleBuffers == 0
                    && finalStats.maximumReceiveBundleBuffers == 0,
                "Fallback receive path must not report bundle CQE diagnostics");
        }

        (void)::close(sockets[1]);
        sockets[1] = -1;
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Receive bundle connection should close after peer EOF");
#endif
    }

    void TestCompletionMultishotAcceptUsesSingleSubmission() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        const int listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP); // 独立 TCP listener
        Require(listenFd >= 0, "Multishot accept listener should open");
        int reuse = 1; // 允许测试失败后快速复跑同一临时端口范围
        Require(::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0,
            "Multishot accept listener should enable reuse address");

        sockaddr_in address{}; // 内核分配 loopback 临时端口
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        Require(::bind(listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "Multishot accept listener should bind");
        Require(::listen(listenFd, SOMAXCONN) == 0, "Multishot accept listener should listen");
        socklen_t addressLength = sizeof(address); // 读取内核选择的端口
        Require(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0,
            "Multishot accept listener should expose bound port");

        auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr);
        Require(poller && poller->Activate(), "Multishot accept Poller should activate");
        std::atomic<int> accepted{ 0 }; // 一个 multishot SQE 应连续产生三次成功 completion
        Require(poller->StartAccept(listenFd, [&accepted](LikesProgram::Net::SocketType clientFd) {
            accepted.fetch_add(1, std::memory_order_acq_rel);
            (void)::close(clientFd);
        }), "Multishot accept should submit listener operation");
        poller->Flush();

        std::array<int, 3> clients{ -1, -1, -1 }; // 连续建连验证 IORING_CQE_F_MORE 生命周期
        for (int& clientFd : clients) {
            clientFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            Require(clientFd >= 0, "Multishot accept client should open");
            Require(::connect(clientFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
                "Multishot accept client should connect");
        }

        std::vector<LikesProgram::Net::Channel*> active; // accept completion 不应伪装成 readiness Channel
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (accepted.load(std::memory_order_acquire) < 3
            && std::chrono::steady_clock::now() < deadline) {
            poller->Poll(10, active);
            poller->Flush();
        }

        Require(accepted.load(std::memory_order_acquire) == 3,
            "One multishot accept should complete three client connections");
        Require(poller->AcceptSubmissionCount() == 1,
            "Three accepts should keep one live multishot submission");
        poller->StopAccept(listenFd);
        poller->Flush();
        for (int clientFd : clients) (void)::close(clientFd);
        (void)::close(listenFd);
#endif
    }

    void TestCompletionBudgetAdaptsAfterLowUtilization() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // wakeup Channel 提供可控的单 CQE 低利用率批次
        const std::size_t initialBudget = loop.GetCompletionStats().completionBudget;
        if (IsEpollBackend(loop.CompletionBackendName())) {
            Require(initialBudget == 0,
                "Epoll completion should not expose an io_uring CQ budget");
            return;
        }
        Require(initialBudget > 64, "Adaptive completion budget should start above its lower bound");
        loop.SetPollTimeout(1000);
        std::thread worker([&loop]() { loop.Start(); });

        std::atomic<int> completed{ 0 }; // 每轮只保留一个 pending task，避免 wakeup 合并成大 batch
        for (int index = 0; index < 70; ++index) {
            loop.PostTask([&completed]() {
                completed.fetch_add(1, std::memory_order_acq_rel);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (completed.load(std::memory_order_acquire) <= index
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(completed.load(std::memory_order_acquire) > index,
                "Adaptive completion budget task should complete");
        }

        loop.Shutdown();
        worker.join();
        const std::size_t finalBudget = loop.GetCompletionStats().completionBudget;
        Require(finalBudget < initialBudget,
            "Sustained low CQ utilization should shrink the completion budget after hysteresis");
        Require(finalBudget >= 64, "Adaptive completion budget should keep its lower safety bound");
#endif
    }

    void TestCompletionLeaseOutlivesPollerClose() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        const pid_t child = ::fork(); // 隔离旧 owner 裸指针导致的关停后 UAF
        Require(child >= 0, "Completion lease close test should fork");
        if (child == 0) {
            try {
                LikesProgram::Net::Buffer retained; // 必须活到 EventLoop/Poller 析构之后
                {
                    LikesProgram::Net::EventLoop loop;
                    int sockets[2] = { -1, -1 }; // 本地 stream 触发真实 multishot provided-buffer
                    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) _exit(2);
                    if (!SetTestNonBlocking(sockets[0]) || !SetTestNonBlocking(sockets[1])) _exit(3);

                    std::atomic<bool> received{ false }; // 等待 lease 已移动到 retained
                    auto connection = std::make_shared<RetainBufferConnection>(
                        sockets[0],
                        &loop,
                        retained,
                        received);
                    loop.AttachConnection(connection);
                    loop.PostTask([connection]() { connection->Start(); });
                    std::thread worker([&loop]() { loop.Start(); });

                    const char payload[] = "retained"; // 关闭后仍需保持可读的 completion 数据
                    if (::send(sockets[1], payload, sizeof(payload) - 1, 0)
                        != static_cast<ssize_t>(sizeof(payload) - 1)) {
                        _exit(4);
                    }

                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (!received.load(std::memory_order_acquire)
                        && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (!received.load(std::memory_order_acquire)) _exit(5);

                    (void)::close(sockets[1]);
                    loop.Shutdown();
                    worker.join();
                    connection.reset();
                }

                if (retained.AsStringView() != "retained") _exit(6);
                retained.RetrieveAll();
                _exit(0);
            }
            catch (...) {
                _exit(7);
            }
        }

        int status = 0; // 父进程只接受正常释放 retained Buffer 的退出结果
        Require(::waitpid(child, &status, 0) == child,
            "Completion lease close test should wait for child");
        Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "BufferLease should keep provided storage alive after Poller close");
#endif
    }

    void TestProvidedBufferGenerationReconfigurationPreservesLeases() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        auto ioUringPoller = std::make_unique<LikesProgram::Net::Internal::IoUringPoller>(nullptr);
        auto* poller = ioUringPoller.get(); // EventLoop 接管所有权，测试只保留 loop 生命周期内观察指针
        LikesProgram::Net::EventLoop loop(std::move(ioUringPoller));
        int sockets[2] = { -1, -1 }; // 真实 stream 驱动切换前后两次 multishot completion
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Provided-buffer generation test should create socketpair");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "Provided-buffer generation sockets should be non-blocking");

        LikesProgram::Net::Buffer oldBuffer; // 保留到旧 ring 注销后，验证 pool 延寿
        LikesProgram::Net::Buffer newBuffer; // 新 generation 产生的 completion
        std::atomic<int> received{ 0 }; // 每次移动完成后向测试线程发布 Buffer
        auto connection = std::make_shared<GenerationCaptureConnection>(
            sockets[0],
            &loop,
            oldBuffer,
            newBuffer,
            received);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const char firstPayload[] = "old-generation"; // 第一次 completion 必须绑定初始 8 KiB generation
        Require(::send(sockets[1], firstPayload, sizeof(firstPayload) - 1, 0)
                == static_cast<ssize_t>(sizeof(firstPayload) - 1),
            "Old generation payload should send");
        const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (received.load(std::memory_order_acquire) < 1
            && std::chrono::steady_clock::now() < firstDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(received.load(std::memory_order_acquire) == 1,
            "Old generation completion should arrive");
        const std::uint8_t* oldAddress = oldBuffer.Peek(); // 旧 pool 存储必须跨切换保持稳定
        const LikesProgram::Net::CompletionStats initialStats = loop.GetCompletionStats(); // 旧代内存规模
        Require(oldBuffer.AsStringView() == "old-generation",
            "Old generation payload should remain readable before reconfiguration");

        std::atomic<bool> requestFinished{ false }; // 请求返回只表示新 generation 注册并进入切换
        std::atomic<bool> requestAccepted{ false }; // 注册失败必须保留旧 generation
        loop.PostTask([poller, &requestFinished, &requestAccepted]() {
            requestAccepted.store(
                poller->ReconfigureProvidedBuffers(4 * 1024, 128),
                std::memory_order_release);
            requestFinished.store(true, std::memory_order_release);
        });
        const auto switchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool transitionFinished = false; // 旧 read terminal CQE 回收后才允许验证新 completion
        while (std::chrono::steady_clock::now() < switchDeadline) {
            const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 原子诊断快照
            transitionFinished = requestFinished.load(std::memory_order_acquire)
                && stats.providedBufferSize == 4 * 1024
                && stats.providedBufferCount == 128
                && stats.retiringProvidedBufferGenerations == 0
                && stats.retainedProvidedBufferGenerations == 1
                && stats.retainedProvidedBufferBytes
                    == initialStats.providedBufferSize * initialStats.providedBufferCount
                && stats.providedBufferReconfigurationCount == 1;
            if (transitionFinished) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(requestAccepted.load(std::memory_order_acquire),
            "Provided-buffer generation request should register a new ring");
        Require(transitionFinished,
            "Provided-buffer generation should retire old reads before reporting completion");
        Require(oldBuffer.Peek() == oldAddress && oldBuffer.AsStringView() == "old-generation",
            "Old lease should remain readable after its ring generation retires");

        std::atomic<bool> rejectedRequestFinished{ false }; // 超限请求必须在注册新 group 前结束
        std::atomic<bool> rejectedRequestAccepted{ true }; // 初值确保测试观察真实写回
        loop.PostTask([poller, &rejectedRequestFinished, &rejectedRequestAccepted]() {
            rejectedRequestAccepted.store(
                poller->ReconfigureProvidedBuffers(64 * 1024, 1024),
                std::memory_order_release);
            rejectedRequestFinished.store(true, std::memory_order_release);
        });
        const auto rejectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!rejectedRequestFinished.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < rejectionDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const LikesProgram::Net::CompletionStats rejectedStats = loop.GetCompletionStats(); // 回滚后快照
        Require(rejectedRequestFinished.load(std::memory_order_acquire)
                && !rejectedRequestAccepted.load(std::memory_order_acquire),
            "Memory-capped provided-buffer request should be rejected");
        Require(rejectedStats.providedBufferSize == 4 * 1024
                && rejectedStats.providedBufferCount == 128
                && rejectedStats.retiringProvidedBufferGenerations == 0
                && rejectedStats.retainedProvidedBufferGenerations == 1
                && rejectedStats.providedBufferReconfigurationCount == 1,
            "Rejected provided-buffer request should preserve the active generation");

        const char secondPayload[] = "new-generation"; // 退役完成后只能由新 group 产生 completion
        Require(::send(sockets[1], secondPayload, sizeof(secondPayload) - 1, 0)
                == static_cast<ssize_t>(sizeof(secondPayload) - 1),
            "New generation payload should send");
        const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (received.load(std::memory_order_acquire) < 2
            && std::chrono::steady_clock::now() < secondDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(received.load(std::memory_order_acquire) == 2,
            "New generation completion should arrive");
        Require(newBuffer.AsStringView() == "new-generation",
            "New generation payload should remain readable");
        Require(newBuffer.Peek() != oldAddress,
            "New generation completion should use different registered storage");

        oldBuffer.RetrieveAll(); // 最后一个旧 lease 释放后 retained 内存必须退出统计
        const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (loop.GetCompletionStats().retainedProvidedBufferGenerations != 0
            && std::chrono::steady_clock::now() < releaseDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(loop.GetCompletionStats().retainedProvidedBufferGenerations == 0,
            "Released old lease should free its retained generation storage");
        newBuffer.RetrieveAll();

        std::atomic<bool> secondSwitchFinished{ false }; // 连续切换用于覆盖再次 cancel 旧 multishot read
        std::atomic<bool> secondSwitchAccepted{ false }; // 40 MiB 新代仍位于总内存上界内
        loop.PostTask([poller, &secondSwitchFinished, &secondSwitchAccepted]() {
            secondSwitchAccepted.store(
                poller->ReconfigureProvidedBuffers(40 * 1024, 1024),
                std::memory_order_release);
            secondSwitchFinished.store(true, std::memory_order_release);
        });
        const auto secondSwitchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool secondTransitionFinished = false; // 旧代无 lease，ring 与 pool 都应在边界内回收
        while (std::chrono::steady_clock::now() < secondSwitchDeadline) {
            const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 第二代切换快照
            secondTransitionFinished = secondSwitchFinished.load(std::memory_order_acquire)
                && stats.providedBufferSize == 40 * 1024
                && stats.providedBufferCount == 1024
                && stats.retiringProvidedBufferGenerations == 0
                && stats.retainedProvidedBufferGenerations == 0
                && stats.providedBufferReconfigurationCount == 2;
            if (secondTransitionFinished) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(secondSwitchAccepted.load(std::memory_order_acquire) && secondTransitionFinished,
            "Second provided-buffer generation should retire the previous read safely");

        std::atomic<bool> noOpFinished{ false }; // 同尺寸请求不得按额外 generation 重复计算内存
        std::atomic<bool> noOpAccepted{ false }; // 幂等请求不增加切换计数，也不触发 cancel
        loop.PostTask([poller, &noOpFinished, &noOpAccepted]() {
            noOpAccepted.store(
                poller->ReconfigureProvidedBuffers(40 * 1024, 1024),
                std::memory_order_release);
            noOpFinished.store(true, std::memory_order_release);
        });
        const auto noOpDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!noOpFinished.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < noOpDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const LikesProgram::Net::CompletionStats noOpStats = loop.GetCompletionStats(); // 幂等请求后稳定快照
        Require(noOpFinished.load(std::memory_order_acquire)
                && noOpAccepted.load(std::memory_order_acquire),
            "Identical provided-buffer dimensions should be an accepted no-op");
        Require(noOpStats.providedBufferSize == 40 * 1024
                && noOpStats.providedBufferCount == 1024
                && noOpStats.retiringProvidedBufferGenerations == 0
                && noOpStats.providedBufferReconfigurationCount == 2,
            "Identical provided-buffer dimensions should not create another generation");

        std::atomic<bool> closingSwitchFinished{ false }; // 发布重配置与连接关闭均已在 issuer 执行
        std::atomic<bool> closingSwitchAccepted{ false }; // 第三代注册成功后立即进入统一关闭路径
        loop.PostTask([poller, connection, &closingSwitchFinished, &closingSwitchAccepted]() {
            closingSwitchAccepted.store(
                poller->ReconfigureProvidedBuffers(4 * 1024, 128),
                std::memory_order_release);
            connection->ForceClose(); // 旧 read cancel 与连接 close 必须共享同一 operation 状态
            closingSwitchFinished.store(true, std::memory_order_release);
        });
        const auto closingSwitchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!closingSwitchFinished.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < closingSwitchDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)::close(sockets[1]);
        loop.Shutdown();
        worker.join();
        Require(closingSwitchFinished.load(std::memory_order_acquire)
                && closingSwitchAccepted.load(std::memory_order_acquire),
            "Connection close during provided-buffer switching should preserve a valid generation");
#endif
    }

    void TestBuffer() {
        LikesProgram::Net::Buffer buffer;
        const char* text = "hello";

        buffer.Append(text, std::strlen(text));
        Require(buffer.ReadableBytes() == 5, "Buffer readable bytes mismatch");
        Require(buffer.AsStringView() == "hello", "Buffer string view mismatch");

        buffer.Consume(2);
        Require(buffer.AsStringView() == "llo", "Buffer consume mismatch");

        buffer.RetrieveAll();
        Require(buffer.ReadableBytes() == 0, "Buffer retrieve all should clear readable bytes");

        std::uint8_t* writeBegin = buffer.PrepareWrite(4);
        std::memcpy(writeBegin, "pong", 4);
        buffer.HasWritten(4);
        Require(buffer.AsStringView() == "pong", "Buffer prepare write mismatch");
    }

    void CountLeaseRelease(void* owner, std::uint32_t) noexcept {
        auto* releaseCount = static_cast<int*>(owner); // 测试观察 RAII 归还次数
        if (releaseCount != nullptr) ++(*releaseCount);
    }

    struct ReturnedBufferRecord {
        std::atomic<int> m_count{ 0 }; // drain 回调执行次数
        std::atomic<std::uint32_t> m_token{ 0 }; // 最后归还的 buffer id
        std::thread::id m_callbackThread; // 回填 ring 的线程应由 Drain 调用方决定
    };

    void RecordReturnedBuffer(void* context, std::uint32_t token) noexcept {
        auto* record = static_cast<ReturnedBufferRecord*>(context); // 测试归还观察器
        if (record == nullptr) return;

        record->m_token.store(token, std::memory_order_release);
        record->m_callbackThread = std::this_thread::get_id();
        record->m_count.fetch_add(1, std::memory_order_release);
    }

    void TestBufferLeaseAdoptionIsNoexcept() {
        // CQE 分派不能因为为 lease 分配控制块而抛异常并中断 EventLoop。
        Require(noexcept(LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            nullptr,
            0,
            nullptr,
            0,
            nullptr)),
            "BufferLease adoption on a receive completion must not throw");
    }

    void TestBufferLeaseMoveAssignmentReleasesExactlyOnce() {
        std::array<std::uint8_t, 1> firstStorage{ 'a' }; // 目标 lease 原 buffer
        std::array<std::uint8_t, 1> secondStorage{ 'b' }; // 移动后接管的源 buffer
        int firstReleaseCount = 0; // 移动赋值时归还
        int secondReleaseCount = 0; // 最终 Reset 时归还

        auto first = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            firstStorage.data(), firstStorage.size(), &firstReleaseCount, 1, &CountLeaseRelease);
        auto second = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            secondStorage.data(), secondStorage.size(), &secondReleaseCount, 2, &CountLeaseRelease);

        first = std::move(second);
        Require(firstReleaseCount == 1, "Move assignment should return the previous target buffer");
        Require(second.Empty(), "Move assignment should leave the source lease empty");
        Require(first.Data() == secondStorage.data(), "Move assignment should transfer the source buffer");
        Require(secondReleaseCount == 0, "Transferred buffer should remain owned by the target lease");

        first.Reset();
        Require(secondReleaseCount == 1, "Reset should return the transferred buffer exactly once");
        first.Reset();
        Require(secondReleaseCount == 1, "Repeated Reset should not return the buffer twice");
    }

    void TestBufferLeaseAvoidsReceiveCopy() {
        std::array<std::uint8_t, 4> storage{ 'p', 'i', 'n', 'g' }; // 模拟 io_uring provided buffer
        int releaseCount = 0; // lease 全部消费时必须只归还一次
        auto lease = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            storage.data(),
            storage.size(),
            &releaseCount,
            7,
            &CountLeaseRelease);

        LikesProgram::Net::Buffer buffer;
        buffer.Append(std::move(lease));
        Require(buffer.Peek() == storage.data(),
            "Buffer should expose a single provided buffer without copying");
        Require(buffer.AsStringView() == "ping", "Buffer lease payload mismatch");
        Require(releaseCount == 0, "Live Buffer lease should not return its provided buffer");

        buffer.Consume(2);
        Require(buffer.Peek() == storage.data() + 2, "Partial lease consume should advance in place");
        Require(releaseCount == 0, "Partial lease consume should keep ownership");
        buffer.RetrieveAll();
        Require(releaseCount == 1, "Consumed Buffer lease should return exactly once");
    }

    void TestOwningBufferCompletionUsesConnectionInputPath() {
        OwningReadConnection connection;
        LikesProgram::Net::Buffer input(0); // 模拟 epoll recv 直接拥有的 payload
        input.Append("epoll-owned", 11);

        LikesProgram::Net::Internal::PollerAccess::CompleteRead(connection, std::move(input));

        Require(connection.Payload().size() == 11,
            "Owning completion should deliver all 11 readable bytes");
        Require(connection.Payload() == "epoll-owned",
            "Owning completion payload should preserve byte order");
    }

    void TestCompletionCallbackExceptionClosesAndReturnsLease() {
        auto poller = std::make_unique<OwnershipPoller>(); // fake Poller 观察统一 StopConnection
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<ThrowingCompletionCallbacksConnection>(fakeFd, &loop);
        loop.AttachConnection(connection);
        connection->Start();

        std::array<std::uint8_t, 4> storage{ 'f', 'a', 'i', 'l' }; // 模拟 provided-buffer CQE
        int releaseCount = 0; // callback 失败关闭时必须立即归还 lease
        auto lease = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            storage.data(), storage.size(), &releaseCount, 12, &CountLeaseRelease);

        bool escaped = false; // completion 分派边界不得看到用户异常
        try {
            LikesProgram::Net::Internal::PollerAccess::CompleteRead(*connection, std::move(lease));
        }
        catch (...) {
            escaped = true;
        }

        Require(!escaped,
            "Completion callback exceptions must not escape the Connection boundary");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Throwing completion callback should close the connection");
        Require(pollerAddress->Stopped(),
            "Throwing completion callback should stop outstanding Poller operations");
        Require(releaseCount == 1,
            "Closing after a completion callback failure should return its buffer lease immediately");

        loop.Shutdown();
        loop.Start(); // 消费 DoClose 安排的延迟 self 释放任务
    }

    void TestCompletionWatermarkExceptionClosesConnection() {
        auto poller = std::make_unique<OwnershipPoller>(); // fake Poller 驱动写水位 completion
        OwnershipPoller* pollerAddress = poller.get();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<ThrowingLowWatermarkConnection>(fakeFd, &loop);
        connection->SetWriteWatermark(4, 2);
        connection->SetMaxPendingWriteBytes(16);
        loop.AttachConnection(connection);
        connection->Start();

        Require(LikesProgram::Net::Internal::PollerAccess::WriteGrowth(*connection, 4),
            "High watermark setup should keep the completion connection open");
        bool escaped = false; // Low watermark 回调异常不得逃回 write CQE 分派
        try {
            LikesProgram::Net::Internal::PollerAccess::WriteDrain(*connection, 1);
        }
        catch (...) {
            escaped = true;
        }

        Require(!escaped,
            "Completion watermark exceptions must not escape PollerAccess::WriteDrain");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Throwing low watermark callback should close the connection");
        Require(pollerAddress->Stopped(),
            "Throwing low watermark callback should cancel Poller operations");

        loop.Shutdown();
        loop.Start(); // 消费关闭路径的延迟 self 释放任务
    }

    void TestCompletionWriteSubmissionObserverExceptionClosesConnection() {
        auto poller = std::make_unique<OwnershipPoller>(); // fake Poller 拒绝接管写 Buffer
        OwnershipPoller* pollerAddress = poller.get();
        pollerAddress->RejectWrites();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        const LikesProgram::Net::SocketType fakeFd = LikesProgram::Net::kInvalidSocket - 1;
        auto connection = std::make_shared<ThrowingWriteErrorConnection>(fakeFd, &loop);
        loop.AttachConnection(connection);
        connection->Start();

        LikesProgram::Net::Buffer payload; // 跨线程任务中模拟 Poller 写提交失败
        payload.Append("fail", 4);
        connection->Send(std::move(payload));
        loop.Shutdown();
        loop.Start();

        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Write submission observer exceptions must still close the connection");
        Require(pollerAddress->Stopped(),
            "Write submission observer failure should stop Poller operations");
    }

    void TestBufferLeaseMaterializesAcrossLifetimeBoundary() {
        std::array<std::uint8_t, 4> storage{ 'p', 'i', 'n', 'g' }; // 模拟即将退出 completion 周期的池内存
        int releaseCount = 0; // 显式物化后应立即归还 provided buffer
        auto lease = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            storage.data(),
            storage.size(),
            &releaseCount,
            8,
            &CountLeaseRelease);

        LikesProgram::Net::Buffer buffer;
        buffer.Append(std::move(lease));
        buffer.Consume(1);
        buffer.Materialize();

        Require(releaseCount == 1, "Materialize should return the provided buffer immediately");
        Require(buffer.AsStringView() == "ing", "Materialize should preserve the unread payload");
        Require(buffer.Peek() != storage.data() + 1,
            "Materialize should detach the Buffer from provided-buffer storage");

        storage[1] = 'x';
        Require(buffer.AsStringView() == "ing",
            "Materialized payload should outlive mutations to the original pool storage");
        buffer.RetrieveAll();
        Require(releaseCount == 1, "Materialized Buffer should not return the same lease twice");
    }

    void TestProvidedBufferPoolReturnsOnDrainThread() {
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(16, 4); // 模拟单 Poller buffer 池
        Require(pool != nullptr, "Provided buffer pool should be created");
        pool->RetainLease(3);
        Require(pool->ActiveLeaseCount() == 1, "Retained lease should increase active pool count");
        Require(pool->CurrentAvailableCount() == 3, "Retained lease should reduce available buffer count");
        Require(pool->MinimumAvailableCount() == 3, "Pool should record its lowest available watermark");
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 首个采样必须产生非零 hold time

        ReturnedBufferRecord record; // 归还必须先排队，不能在业务线程直接触碰 ring
        std::thread releaser([pool]() {
            pool->ReleaseLease(3);
        });
        releaser.join();

        Require(record.m_count.load(std::memory_order_acquire) == 0,
            "Cross-thread lease release should not call the ring recycler directly");
        Require(pool->ActiveLeaseCount() == 0, "Released lease should leave the active pool count");
        Require(pool->PendingReturnCount() == 1, "Released lease should wait in the issuer return queue");
        Require(pool->CurrentAvailableCount() == 3,
            "Released lease should remain unavailable until the issuer restores its ring entry");
        const std::thread::id drainThread = std::this_thread::get_id(); // 当前线程模拟 EventLoop issuer
        pool->DrainReturned(&record, &RecordReturnedBuffer);
        Require(record.m_count.load(std::memory_order_acquire) == 1,
            "Drain should recycle one queued provided buffer");
        Require(record.m_token.load(std::memory_order_acquire) == 3,
            "Drain should preserve the returned buffer id");
        Require(record.m_callbackThread == drainThread,
            "Provided buffer recycling should execute on the EventLoop drain thread");
        Require(pool->PendingReturnCount() == 0, "Drained token should leave the return queue");
        Require(pool->CurrentAvailableCount() == 4, "Drained token should restore available buffer count");
        Require(pool->LeaseHoldSampleCount() == 1, "Pool should sample the first lease hold duration");
        Require(pool->TotalLeaseHoldNanoseconds() > 0,
            "Sampled lease hold duration should contribute to the total");
        Require(pool->MaximumLeaseHoldNanoseconds() > 0,
            "Pool should record a maximum sampled lease hold duration");
        pool->Close();
    }

    void TestProvidedBufferPoolConcurrentReturns() {
        constexpr std::uint32_t kBufferCount = 32; // 一半租出、一半留在 ring 作为水位对照
        constexpr std::uint32_t kLeaseCount = 16;  // 每个释放线程持有唯一 token
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(64, kBufferCount); // 并发归还池
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) pool->RetainLease(token);

        std::vector<std::thread> releasers; // 模拟 lease 在多个业务线程同时析构
        releasers.reserve(kLeaseCount);
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) {
            releasers.emplace_back([pool, token]() { pool->ReleaseLease(token); });
        }
        for (std::thread& releaser : releasers) releaser.join();

        Require(pool->ActiveLeaseCount() == 0,
            "Concurrent releases should clear every active lease");
        Require(pool->PendingReturnCount() == kLeaseCount,
            "Concurrent releases should queue every unique token exactly once");
        Require(pool->CurrentAvailableCount() == kBufferCount - kLeaseCount,
            "Pending concurrent returns must remain unavailable before issuer drain");

        ReturnedBufferRecord record; // issuer 批次只验证回调发生，精确 token 已由单 token 用例覆盖
        pool->DrainReturned(&record, &RecordReturnedBuffer);
        Require(record.m_count.load(std::memory_order_acquire) == kLeaseCount,
            "Issuer drain should restore every concurrently returned token");
        Require(pool->CurrentAvailableCount() == kBufferCount,
            "Issuer drain should restore the full buffer-ring water level");
        pool->Close();

        auto* closingPool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(
            64,
            kBufferCount); // Close 后由活动 lease 延长池生命周期
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) closingPool->RetainLease(token);
        closingPool->Close();
        std::vector<std::thread> lateReleasers; // 晚到 release 只能减少引用，不能回填 ring
        lateReleasers.reserve(kLeaseCount);
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) {
            lateReleasers.emplace_back([closingPool, token]() { closingPool->ReleaseLease(token); });
        }
        for (std::thread& releaser : lateReleasers) releaser.join();
    }

    void TestProvidedBufferPoolOutlivesPollerClose() {
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(16, 4); // 关闭后由 lease 引用延长存储
        Require(pool != nullptr, "Provided buffer pool should be created for close test");
        std::uint8_t* retained = pool->BufferAddress(1);
        Require(retained != nullptr, "Provided buffer address should be available before close");
        retained[0] = 'z';

        pool->RetainLease(1);
        pool->RetainLease(2);
        pool->Close();
        Require(retained[0] == 'z', "Outstanding lease should keep pool storage alive after Poller close");
        Require(pool->CurrentAvailableCount() == 0,
            "Closed pool should expose no buffers as available to a released ring");
        Require(pool->PendingReturnCount() == 0,
            "Closing a pool should discard every pending ring return");

        ReturnedBufferRecord record; // 关闭后的 token 不得再回填已释放的 ring
        pool->ReleaseLease(1);
        pool->DrainReturned(&record, &RecordReturnedBuffer);
        Require(record.m_count.load(std::memory_order_acquire) == 0,
            "Closed pool should discard late buffer returns without touching the ring");
        pool->ReleaseLease(2);
    }

    // CQ overflow 由 completion budget 消化，不得扩大仍有充足水位的 provided-buffer ring。
    void TestProvidedBufferPolicySeparatesCqCongestionFromBufferPressure() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 单 Poller 的 generation 迟滞状态
        ProvidedBufferPolicy::Observation observation{}; // 只有 CQ 拥塞，没有 ENOBUFS 或 lease 压力
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 128;
        observation.currentAvailable = observation.bufferCount;
        observation.activeLeases = 0;
        observation.cqOverflowDelta = 1;
        observation.receivedBytes = observation.bufferSize * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        for (std::uint32_t sample = 0; sample < 4; ++sample) {
            const auto recommendation = policy.Observe(observation); // 重复拥塞也只调节 CQ 消费预算
            Require(!recommendation.changed
                    && recommendation.bufferSize == observation.bufferSize
                    && recommendation.bufferCount == observation.bufferCount,
                "CQ congestion must not be treated as provided-buffer starvation");
        }
    }

    // SQ 暂满后只重试尚未取消且状态机不再需要的 operation。
    void TestOperationCancellationPolicyRetriesWithoutDuplicates() {
        using LikesProgram::Net::Internal::NeedsOperationCancellation;
        using LikesProgram::Net::Internal::RetainCancellationRequest;
        Require(NeedsOperationCancellation(true, false, false),
            "Unwanted outstanding operation should retry cancellation");
        Require(!NeedsOperationCancellation(true, true, false),
            "Operation with a submitted cancel SQE should not duplicate cancellation");
        Require(!NeedsOperationCancellation(true, false, true),
            "Wanted operation should remain active");
        Require(!NeedsOperationCancellation(false, false, false),
            "Retired operation should not submit cancellation");
        Require(RetainCancellationRequest(true, true),
            "Live multishot operation should retain a submitted cancel latch");
        Require(!RetainCancellationRequest(true, false),
            "Terminal multishot completion should clear the cancel latch");
    }

    // 关闭回调必须同时匹配 fd 索引中的对象身份，不能误删复用 fd 的新连接。
    void TestConnectionIdentityPolicyRejectsStaleCloseCallback() {
        using LikesProgram::Net::Internal::IsConnectionLeaseable;
        using LikesProgram::Net::Internal::MatchesWeakIdentity;
        auto oldConnection = std::make_shared<int>(1); // 模拟回调晚到的旧连接
        auto replacement = std::make_shared<int>(2); // 模拟相同 fd 当前登记的新连接
        std::weak_ptr<int> registered = replacement; // 池索引持有的当前 weak 身份

        Require(MatchesWeakIdentity(registered, replacement.get()),
            "Current connection should match its pool identity");
        Require(!MatchesWeakIdentity(registered, oldConnection.get()),
            "Stale close callback should not match a replacement connection");
        Require(IsConnectionLeaseable(true, true),
            "Connected object with a valid socket should be leaseable");
        Require(!IsConnectionLeaseable(false, true)
                && !IsConnectionLeaseable(true, false),
            "Closed or invalid-socket object should not enter the pool index");
    }

    void TestBufferChainOwnsAndConsumesLeaseSegments() {
        std::array<std::uint8_t, 3> firstStorage{ 'o', 'n', 'e' }; // 第一段 provided buffer
        std::array<std::uint8_t, 3> secondStorage{ 't', 'w', 'o' }; // 第二段 provided buffer
        int firstReleaseCount = 0; // 第一段跨段消费后应立即归还
        int secondReleaseCount = 0; // 第二段物化时应归还
        auto first = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            firstStorage.data(), firstStorage.size(), &firstReleaseCount, 9, &CountLeaseRelease);
        auto second = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            secondStorage.data(), secondStorage.size(), &secondReleaseCount, 10, &CountLeaseRelease);

        LikesProgram::Net::BufferChain chain;
        chain.Append(std::move(first));
        chain.Append(std::move(second));
        Require(chain.SegmentCount() == 2, "BufferChain should preserve two owned lease segments");
        Require(chain.ReadableBytes() == 6, "BufferChain readable bytes should sum all segments");
        Require(chain.Segment(0).Data() == firstStorage.data(),
            "BufferSlice should borrow the first provided-buffer without copying");
        Require(chain.Segment(1).AsStringView() == "two",
            "BufferSlice should expose the second segment payload");

        chain.Consume(4);
        Require(firstReleaseCount == 1, "Consuming the first segment should return its lease immediately");
        Require(secondReleaseCount == 0, "Partially consumed second segment should remain leased");
        Require(chain.SegmentCount() == 1, "Consumed empty segments should leave the readable chain view");
        Require(chain.Segment(0).AsStringView() == "wo",
            "Cross-segment consume should preserve the unread suffix");

        chain.Materialize();
        Require(secondReleaseCount == 1, "Materializing a chain should release its remaining leases");
        Require(chain.Segment(0).Data() != secondStorage.data() + 1,
            "Materialized chain segment should detach from provided-buffer storage");
        secondStorage[1] = 'x';
        Require(chain.Segment(0).AsStringView() == "wo",
            "Materialized chain payload should outlive original pool storage mutations");
        chain.Clear();
        Require(chain.Empty(), "Cleared BufferChain should be empty");
        Require(secondReleaseCount == 1, "Clearing a materialized chain should not return a lease twice");

        LikesProgram::Net::Buffer prefix; // 目标链已有的首段
        prefix.Append("one", 3);
        LikesProgram::Net::Buffer suffix; // 来源链待移动的尾段
        suffix.Append("two", 3);
        LikesProgram::Net::BufferChain destination; // 接收另一条链所有权的目标
        LikesProgram::Net::BufferChain source; // 移动后必须清空的来源
        destination.Append(std::move(prefix));
        source.Append(std::move(suffix));
        destination.Append(std::move(source));
        Require(source.Empty(), "Moving a BufferChain should empty the source");
        Require(destination.SegmentCount() == 2,
            "Moving a BufferChain should preserve existing and incoming segments");
        Require(destination.Segment(0).AsStringView() == "one"
            && destination.Segment(1).AsStringView() == "two",
            "Moving a BufferChain should preserve segment order without merging");
    }

    void TestConnectionMovesBufferIntoPollerWrite() {
        std::array<std::uint8_t, 4> storage{ 's', 'e', 'n', 'd' }; // 模拟无需复制的应用 payload
        int releaseCount = 0; // Poller 完成写入前必须继续持有 lease
        auto lease = LikesProgram::Net::Internal::BufferLeaseAccess::Adopt(
            storage.data(), storage.size(), &releaseCount, 11, &CountLeaseRelease);
        LikesProgram::Net::Buffer payload;
        payload.Append(std::move(lease));

        auto poller = std::make_unique<OwnershipPoller>();
        OwnershipPoller* observer = poller.get(); // EventLoop 接管后保留非拥有观察指针
        LikesProgram::Net::EventLoop loop(std::move(poller));
        int sockets[2] = { -1, -1 }; // Connection::Start 需要有效 fd 才进入 direct Poller I/O
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Ownership send socketpair should open");

        auto connection = std::make_shared<OwnershipSendConnection>(sockets[0], &loop, payload);
        loop.AttachConnection(connection);
        connection->Start();

        Require(payload.ReadableBytes() == 0, "Send(Buffer&&) should leave the source Buffer empty");
        Require(observer->PendingWrite().Peek() == storage.data(),
            "Poller should receive the original Buffer storage without copying payload bytes");
        Require(releaseCount == 0, "Poller should retain Buffer ownership until write completion");
        observer->CompleteWrite();
        Require(releaseCount == 1, "Completing the Poller write should release the payload exactly once");

        connection->ForceClose();
        (void)::close(sockets[1]);
    }

    // connected UDP 的默认 Send 必须让 Poller 选择无 msg_name 的 send operation。
    void TestConnectedDatagramSendUsesConnectedSocket() {
#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // connect 不要求远端实际监听
        Require(fd >= 0, "Connected UDP contract socket should open");
        const LikesProgram::Net::Address remote("127.0.0.1", 9); // discard 端口只用于固定 socket peer
        Require(::connect(fd, remote.SockAddr(), remote.Length()) == 0,
            "Connected UDP contract socket should bind a default peer");

        auto poller = std::make_unique<OwnershipPoller>(); // 捕获 Connection 交给 Poller 的 peer 语义
        OwnershipPoller* pollerAddress = poller.get(); // EventLoop 接管所有权后的只读观察地址
        LikesProgram::Net::EventLoop loop(std::move(poller));
        auto connection = std::make_shared<ConnectedDatagramSendConnection>(fd, &loop);
        loop.AttachConnection(connection);
        const int channelCountBeforeStart = pollerAddress->ChannelAddCount(); // 只含 EventLoop wakeup
        connection->Start();

        Require(pollerAddress->QueueWriteCount() == 1,
            "Connected UDP OnConnected should queue exactly one datagram");
        Require(!pollerAddress->DatagramPeer().IsValid(),
            "Connected UDP Send should not attach an explicit sendmsg peer");
        Require(pollerAddress->DatagramWrite().AsStringView() == "ping",
            "Connected UDP Send should preserve the complete payload");
        Require(pollerAddress->ChannelAddCount() == channelCountBeforeStart,
            "Connected UDP should not create a Channel-backed I/O path");

        connection->ForceClose();
        loop.Shutdown();
        loop.Start(); // 处理关闭任务并让 fake Poller 释放连接快照
#endif
    }

    // connected UDP Client 必须在 OnConnected 前创建会话、排首个 flight 并登记两个 timer。
    void TestDtlsClientStartsOnConnectedUdp() {
#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // connect 只建立默认 peer
        Require(fd >= 0, "DTLS client startup socket should open");
        const LikesProgram::Net::Address remote("127.0.0.1", 9); // fake Poller 不执行真实发送
        Require(::connect(fd, remote.SockAddr(), remote.Length()) == 0,
            "DTLS client startup socket should have a default peer");

        auto poller = std::make_unique<OwnershipPoller>(); // 记录首个 flight 与 timer 顺序
        OwnershipPoller* pollerAddress = poller.get();
        pollerAddress->EnableTimeoutCapture();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        DtlsClientStartupState state; // 同一 issuer 线程内无需额外同步
        auto connection = std::make_shared<DtlsClientStartupConnection>(fd, &loop, state);
        loop.AttachConnection(connection);
        connection->Start();

        Require(state.m_onSecureLayerReady == 1 && state.m_onConnected == 1,
            "DTLS client should preserve secure-ready and connected callbacks");
        Require(state.m_createdEngines == 1 && state.m_lastError == 0,
            "DTLS client should create exactly one Engine without session error");
        Require(pollerAddress->DatagramPayloads() == std::vector<std::string>{ "client-flight" },
            "DTLS client should queue the first flight as one complete datagram");
        Require(pollerAddress->ScheduledDelays() == std::vector<std::int64_t>{ 30000, 25 },
            "DTLS client should arm handshake deadline before Engine retransmit timer");
        Require(state.m_order == std::vector<std::string>{
                "secure-ready", "factory-create", "initial-flight", "connected" },
            "DTLS client startup callback order should remain deterministic");
        const auto stats = connection->GetDtlsSessionStats(); // 跨线程可读的标量快照
        Require(stats.pendingSessions == 1 && stats.createdSessions == 1,
            "DTLS client startup should publish one pending session");

        connection->ForceClose();
        loop.Shutdown();
        loop.Start(); // 执行关闭任务并取消 fake timer
#endif
    }

    // DTLS 配置必须拒绝 TCP，Client Factory 也必须拒绝 unconnected UDP。
    void TestDtlsRejectsInvalidTransportConfigurations() {
        auto tcpPoller = std::make_unique<OwnershipPoller>(); // setter 阶段不启动 Poller
        LikesProgram::Net::EventLoop tcpLoop(std::move(tcpPoller));
        LikesProgram::Net::Connection tcpConnection(
            LikesProgram::Net::kInvalidSocket - 1,
            &tcpLoop);
        bool rejectedTcp = false; // TCP setter 必须同步报告配置错误
        try {
            tcpConnection.SetDtlsEngineFactory(LikesProgram::Net::DtlsEngineFactory(
                LikesProgram::Net::DtlsRole::Client,
                [](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t)
                    -> std::unique_ptr<LikesProgram::Net::DtlsEngine> {
                    return {};
                }));
        }
        catch (const std::invalid_argument&) {
            rejectedTcp = true;
        }
        Require(rejectedTcp, "DTLS configuration should reject TCP before Start");

#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 保持未 connect 状态
        Require(fd >= 0, "Unconnected DTLS client socket should open");
        auto udpPoller = std::make_unique<OwnershipPoller>();
        OwnershipPoller* pollerAddress = udpPoller.get();
        LikesProgram::Net::EventLoop udpLoop(std::move(udpPoller));
        DtlsClientStartupState state; // 记录 EINVAL 与回调抑制
        auto connection = std::make_shared<DtlsClientStartupConnection>(fd, &udpLoop, state);
        udpLoop.AttachConnection(connection);
        connection->Start();
        Require(state.m_onSecureLayerReady == 1 && state.m_onConnected == 0,
            "Unconnected DTLS client should stop before OnConnected");
        Require(state.m_createdEngines == 0 && state.m_lastError == EINVAL,
            "Unconnected DTLS client should fail with EINVAL before Factory Create");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed
                && pollerAddress->Stopped(),
            "Invalid DTLS client configuration should stop Poller I/O");
        udpLoop.Shutdown();
        udpLoop.Start();
#endif
    }

    // unconnected Server 必须按 canonical peer 隔离 Engine、握手、乱序状态与明文交付。
    void TestDtlsServerRoutesInterleavedPeers() {
#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // fake Poller 只要求有效 unconnected fd
        Require(fd >= 0, "DTLS server routing socket should open");
        auto poller = std::make_unique<OwnershipPoller>(); // 记录每个 peer 的响应数据报目标
        OwnershipPoller* pollerAddress = poller.get();
        pollerAddress->EnableTimeoutCapture();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        DtlsServerRoutingState state; // 同一 issuer 线程内的两-peer 状态
        auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state);
        loop.AttachConnection(connection);
        connection->Start();

        const LikesProgram::Net::Address peerA("127.0.0.1", 51001); // 首个独立 peer
        const LikesProgram::Net::Address peerB("127.0.0.1", 51002); // 第二个独立 peer
        const auto deliver = [&connection](
            const LikesProgram::Net::Address& peer,
            const char* bytes) {
            LikesProgram::Net::Buffer ciphertext(0); // 一个完整 fake ciphertext 数据报
            const std::size_t length = std::strlen(bytes);
            ciphertext.Append(bytes, length);
            LikesProgram::Net::Internal::PollerAccess::CompleteDatagram(
                *connection,
                ciphertext,
                peer,
                length,
                false);
        };

        deliver(peerA, "cookie");
        deliver(peerA, "fragment-2"); // A 的乱序状态不得进入 B Engine
        deliver(peerB, "cookie");
        deliver(peerB, "valid-cookie");
        deliver(peerB, "fragment-1");
        deliver(peerA, "valid-cookie");
        deliver(peerA, "data:A-data");
        deliver(peerB, "data:B-data");
        deliver(peerA, "data:A-data"); // duplicate/replay 不得重复交付

        const std::string keyA = peerA.ToString(); // 测试观察键
        const std::string keyB = peerB.ToString();
        Require(state.m_createdByPeer[keyA] == 1 && state.m_createdByPeer[keyB] == 1,
            "DTLS server should create one Engine per peer");
        Require(state.m_plaintextByPeer[keyA] == "A-data"
                && state.m_plaintextByPeer[keyB] == "B-data",
            "DTLS server should route plaintext to the originating peer");
        Require(state.m_plaintextCallbacks[keyA] == 1 && state.m_plaintextCallbacks[keyB] == 1,
            "DTLS duplicate input should not duplicate plaintext callbacks");
        Require(state.m_handshakeCallbacks[keyA] == 1 && state.m_handshakeCallbacks[keyB] == 1,
            "Each DTLS peer should publish handshake completion once");
        Require(state.m_sessionErrors.empty(), "Interleaved peers should not cross-contaminate errors");
        Require(connection->GetDtlsSessionStats().activeSessions == 2,
            "Both peer sessions should be Active before peer-local close");
        Require(pollerAddress->DatagramPeers().size() == 4
                && pollerAddress->DatagramPeers()[0].ToString() == keyA
                && pollerAddress->DatagramPeers()[1].ToString() == keyB
                && pollerAddress->DatagramPeers()[2].ToString() == keyB
                && pollerAddress->DatagramPeers()[3].ToString() == keyA,
            "Handshake ciphertext should preserve each explicit peer target");

        deliver(peerA, "close");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Connected,
            "Closing peer A must leave the shared UDP Connection running");
        Require(connection->GetDtlsSessionStats().activeSessions == 1,
            "Closing peer A must leave peer B Active");

        connection->ForceClose();
        loop.Shutdown();
        loop.Start();
#endif
    }

    // Server 资源上限与用户/Engine 故障必须严格保持 peer-local。
    void TestDtlsServerBoundsAndIsolatesPeerFailures() {
#ifndef _WIN32
        const LikesProgram::Net::Address peerA("127.0.0.1", 52001); // 故障或首个容量 peer
        const LikesProgram::Net::Address peerB("127.0.0.1", 52002); // 必须保持可用的其他 peer
        const auto deliver = [](const std::shared_ptr<DtlsServerRoutingConnection>& connection,
            const LikesProgram::Net::Address& peer,
            const char* bytes) {
            LikesProgram::Net::Buffer ciphertext(0); // 一个完整 fake ciphertext 数据报
            const std::size_t length = std::strlen(bytes);
            ciphertext.Append(bytes, length);
            LikesProgram::Net::Internal::PollerAccess::CompleteDatagram(
                *connection, ciphertext, peer, length, false);
        };

        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 单会话 cap fixture
            Require(fd >= 0, "DTLS server cap socket should open");
            auto poller = std::make_unique<OwnershipPoller>();
            OwnershipPoller* pollerAddress = poller.get();
            pollerAddress->EnableTimeoutCapture();
            LikesProgram::Net::EventLoop loop(std::move(poller));
            DtlsServerRoutingState state;
            auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state, 1);
            loop.AttachConnection(connection);
            connection->Start();
            deliver(connection, peerA, "cookie");
            deliver(connection, peerB, "cookie");
            const auto stats = connection->GetDtlsSessionStats(); // cap 后稳定快照
            Require(state.m_createdByPeer[peerA.ToString()] == 1
                    && state.m_createdByPeer[peerB.ToString()] == 0,
                "DTLS server cap should avoid creating an over-limit peer Engine");
            Require(stats.createdSessions == 1 && stats.droppedNewPeers == 1
                    && state.m_sessionErrors.empty(),
                "Over-limit unknown peer should be dropped silently and counted once");
            connection->ForceClose();
            loop.Shutdown();
            loop.Start();
        }

        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // peer-local failure fixture
            Require(fd >= 0, "DTLS server isolation socket should open");
            auto poller = std::make_unique<OwnershipPoller>();
            OwnershipPoller* pollerAddress = poller.get();
            pollerAddress->EnableTimeoutCapture();
            LikesProgram::Net::EventLoop loop(std::move(poller));
            DtlsServerRoutingState state;
            auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state);
            loop.AttachConnection(connection);
            connection->Start();
            deliver(connection, peerA, "valid-cookie");
            deliver(connection, peerB, "valid-cookie");
            state.m_throwPlaintextPeer = peerA.ToString();
            deliver(connection, peerA, "data:A-fails");
            deliver(connection, peerB, "data:B-survives");
            Require(state.m_sessionErrors[peerA.ToString()] == 1
                    && state.m_plaintextByPeer[peerB.ToString()] == "B-survives",
                "OnDatagram exception should close only peer A and preserve peer B");
            Require(connection->GetState() == LikesProgram::Net::Connection::State::Connected
                    && connection->GetDtlsSessionStats().activeSessions == 1,
                "Peer-local callback failure should preserve shared Connection and peer B");

            deliver(connection, peerB, "malformed");
            Require(state.m_sessionErrors[peerB.ToString()] == 1
                    && connection->GetDtlsSessionStats().activeSessions == 0,
                "Malformed Engine result should close only its own peer session");
            Require(connection->GetState() == LikesProgram::Net::Connection::State::Connected,
                "Server session errors must not close the shared UDP Connection");
            connection->ForceClose();
            loop.Shutdown();
            loop.Start();
        }
#endif
    }

    // timer generation、精确 MTU、输入消费与 close-flight 顺序必须保持 peer-local。
    void TestDtlsTimersMtuAndCloseSemantics() {
#ifndef _WIN32
        const LikesProgram::Net::Address peerA("127.0.0.1", 53001); // timer/EPROTO peer
        const LikesProgram::Net::Address peerB("127.0.0.1", 53002); // 隔离存活/EMSGSIZE peer
        const LikesProgram::Net::Address peerC("127.0.0.1", 53003); // close-flight peer
        const LikesProgram::Net::Address peerD("127.0.0.1", 53004); // existing truncated peer
        const auto deliver = [](const std::shared_ptr<DtlsServerRoutingConnection>& connection,
            const LikesProgram::Net::Address& peer,
            const char* bytes,
            bool truncated = false) {
            LikesProgram::Net::Buffer ciphertext(0); // 一个完整或显式截断的 fake ciphertext
            const std::size_t length = std::strlen(bytes);
            ciphertext.Append(bytes, length);
            LikesProgram::Net::Internal::PollerAccess::CompleteDatagram(
                *connection, ciphertext, peer, length, truncated);
        };

        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // timer generation fixture
            Require(fd >= 0, "DTLS timer socket should open");
            auto poller = std::make_unique<OwnershipPoller>();
            OwnershipPoller* pollerAddress = poller.get();
            pollerAddress->EnableTimeoutCapture();
            LikesProgram::Net::EventLoop loop(std::move(poller));
            DtlsServerRoutingState state;
            auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state);
            connection->SetDtlsSessionIdleTimeout(std::chrono::milliseconds(100));
            loop.AttachConnection(connection);
            connection->Start();

            deliver(connection, peerA, "cookie");
            deliver(connection, peerA, "cookie"); // cookie traffic 不得刷新固定 handshake deadline
            deliver(connection, peerA, "arm");
            deliver(connection, peerA, "arm"); // 替换 Engine timer 并取消旧 generation
            Require(!pollerAddress->FireTimeoutAt(1) && pollerAddress->FireTimeoutAt(2),
                "Replaced retransmit timer should suppress the stale callback");
            Require(state.m_retransmitCallbacks[peerA.ToString()] == 1,
                "Current retransmit timer should invoke only peer A Engine once");

            deliver(connection, peerB, "valid-cookie");
            deliver(connection, peerA, "valid-cookie");
            deliver(connection, peerA, "data:A-data"); // Active progress 刷新 idle timer
            const std::size_t delaysAfterProgress = pollerAddress->ScheduledDelays().size();
            deliver(connection, peerA, "data:A-data"); // duplicate/replay 无输出，不刷新
            Require(pollerAddress->ScheduledDelays().size() == delaysAfterProgress,
                "Duplicate/no-output input should not refresh DTLS idle timeout");
            Require(pollerAddress->ScheduledDelays()
                    == std::vector<std::int64_t>{ 30000, 10, 10, 30000, 100, 100, 100 },
                "DTLS timer order should preserve fixed deadlines and progress-only idle refresh");
            Require(!pollerAddress->FireTimeoutAt(5) && pollerAddress->FireTimeoutAt(6),
                "Refreshed idle timer should reject its stale generation and fire the current one");
            Require(state.m_lastSessionError[peerA.ToString()] == ETIMEDOUT
                    && connection->GetDtlsSessionStats().activeSessions == 1,
                "Peer A idle expiry should leave peer B Active");
            Require(connection->GetDtlsSessionStats().retransmitTimeouts == 1
                    && connection->GetState() == LikesProgram::Net::Connection::State::Connected,
                "Peer-local timers should preserve the shared UDP Connection");
            connection->ForceClose();
            loop.Shutdown();
            loop.Start();
        }

        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // MTU/result validation fixture
            Require(fd >= 0, "DTLS MTU socket should open");
            auto poller = std::make_unique<OwnershipPoller>();
            OwnershipPoller* pollerAddress = poller.get();
            pollerAddress->EnableTimeoutCapture();
            LikesProgram::Net::EventLoop loop(std::move(poller));
            DtlsServerRoutingState state;
            auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state);
            connection->SetDtlsSessionIdleTimeout(std::chrono::milliseconds(0));
            loop.AttachConnection(connection);
            connection->Start();

            deliver(connection, peerA, "unknown-truncated", true);
            Require(connection->GetDtlsSessionStats().createdSessions == 0,
                "Unknown truncated ciphertext must not create a DTLS Engine");
            deliver(connection, peerA, "valid-cookie");
            deliver(connection, peerA, "mtu-1200");
            deliver(connection, peerA, "unconsumed");
            deliver(connection, peerB, "valid-cookie");
            deliver(connection, peerB, "mtu-1201");
            deliver(connection, peerC, "valid-cookie");
            deliver(connection, peerC, "close-flight");
            deliver(connection, peerD, "valid-cookie");
            deliver(connection, peerD, "truncated", true);

            const auto& payloads = pollerAddress->DatagramPayloads(); // 已通过校验并实际排队的输出
            Require(std::any_of(payloads.begin(), payloads.end(), [](const std::string& payload) {
                    return payload.size() == 1200;
                }),
                "Exact 1200-byte DTLS ciphertext should be queued");
            Require(std::none_of(payloads.begin(), payloads.end(), [](const std::string& payload) {
                    return payload.size() == 1201;
                }),
                "1201-byte DTLS ciphertext should be rejected before Poller queueing");
            Require(std::find(payloads.begin(), payloads.end(), "close-flight") != payloads.end(),
                "CloseSession ciphertext must be queued before the peer enters drain-only Closing");
            Require(state.m_lastSessionError[peerA.ToString()] == EPROTO
                    && state.m_lastSessionError[peerB.ToString()] == EMSGSIZE
                    && state.m_lastSessionError[peerD.ToString()] == EMSGSIZE,
                "DTLS malformed input and MTU/truncation must use stable peer-local errors");
            Require(connection->GetDtlsSessionStats().createdSessions == 4
                    && connection->GetDtlsSessionStats().activeSessions == 0,
                "All validation fixtures should close only their own sessions");
            Require(connection->GetState() == LikesProgram::Net::Connection::State::Connected,
                "DTLS validation errors and close-flight must preserve unconnected Server socket");
            connection->ForceClose();
            loop.Shutdown();
            loop.Start();
        }
#endif
    }

    // Closing peer 必须在自己的最后一个 ciphertext CQE 后移除，不等待其他 peer 排空。
    void TestDtlsPeerCiphertextRetiresIndependently() {
#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // per-peer drain fixture
        Require(fd >= 0, "DTLS peer drain socket should open");
        auto poller = std::make_unique<OwnershipPoller>();
        OwnershipPoller* pollerAddress = poller.get();
        pollerAddress->EnableTimeoutCapture();
        LikesProgram::Net::EventLoop loop(std::move(poller));
        DtlsServerRoutingState state;
        auto connection = std::make_shared<DtlsServerRoutingConnection>(fd, &loop, state);
        connection->SetDtlsSessionIdleTimeout(std::chrono::milliseconds(0));
        loop.AttachConnection(connection);
        connection->Start();
        const LikesProgram::Net::Address peerA("127.0.0.1", 54001); // 先关闭并独立排空
        const LikesProgram::Net::Address peerB("127.0.0.1", 54002); // 保持未回收写的 Active peer
        const auto deliver = [&connection](const LikesProgram::Net::Address& peer, const char* bytes) {
            LikesProgram::Net::Buffer ciphertext(0); // 完整 fake ciphertext 输入
            const std::size_t length = std::strlen(bytes);
            ciphertext.Append(bytes, length);
            LikesProgram::Net::Internal::PollerAccess::CompleteDatagram(
                *connection, ciphertext, peer, length, false);
        };

        deliver(peerA, "valid-cookie"); // datagram index 0，A active flight
        deliver(peerB, "valid-cookie"); // datagram index 1，B active flight
        deliver(peerA, "close-flight"); // datagram index 2，A final close flight
        deliver(peerA, "cookie"); // Closing key 仍保留，不得创建第二个 A Engine
        Require(state.m_createdByPeer[peerA.ToString()] == 1
                && state.m_closedCallbacks[peerA.ToString()] == 0,
            "DTLS closing key should remain reserved before its ciphertext CQEs retire");

        Require(pollerAddress->CompleteDatagramWriteAt(0),
            "First peer A ciphertext CQE should retire once");
        Require(state.m_closedCallbacks[peerA.ToString()] == 0,
            "Peer A should remain Closing until its final close-flight CQE");
        Require(pollerAddress->CompleteDatagramWriteAt(2),
            "Final peer A close-flight CQE should retire once");
        Require(state.m_closedCallbacks[peerA.ToString()] == 1,
            "Peer A should close immediately after its own final CQE");
        Require(connection->GetDtlsSessionStats().activeSessions == 1
                && connection->GetState() == LikesProgram::Net::Connection::State::Connected,
            "Peer B should remain Active without completing its ciphertext write");
        Require(!pollerAddress->CompleteDatagramWriteAt(2),
            "Duplicate fake CQE must not underflow peer A ciphertext cost");

        deliver(peerA, "cookie");
        Require(state.m_createdByPeer[peerA.ToString()] == 2,
            "Peer A key should be reusable only after its final ciphertext CQE");
        connection->ForceClose();
        loop.Shutdown();
        loop.Start();
#endif
    }

    // 真实 io_uring connected UDP Client 覆盖 timer、双向多数据报与 graceful close。
    void TestRealDtlsConnectedClientDataPath() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING) && !defined(_WIN32)
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 两端均由内核分配回环端口
        const int peerFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程模拟远端 DTLS peer
        Require(peerFd >= 0 && ::bind(peerFd, any.SockAddr(), any.Length()) == 0,
            "Real DTLS client peer socket should bind");
        Require(SetTestNonBlocking(peerFd), "Real DTLS client peer should be nonblocking");
        const LikesProgram::Net::Address peerAddress =
            LikesProgram::Net::Address::GetLocalAddress(peerFd); // connected Client 固定远端

        const int clientFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
        Require(clientFd >= 0
                && ::connect(clientFd, peerAddress.SockAddr(), peerAddress.Length()) == 0,
            "Real DTLS client socket should connect to its peer");
        Require(SetTestNonBlocking(clientFd), "Real DTLS client socket should be nonblocking");
        const LikesProgram::Net::Address clientAddress =
            LikesProgram::Net::Address::GetLocalAddress(clientFd); // raw peer 回送目标

        LikesProgram::Net::EventLoop loop; // 真实 io_uring UDP completion issuer
        RealDtlsClientState state;
        auto connection = std::make_shared<RealDtlsClientConnection>(clientFd, &loop, state);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto receiveDatagrams = [peerFd](std::size_t expected, std::chrono::seconds timeout) {
            std::vector<std::string> payloads; // 按真实 UDP 到达顺序保存完整数据报
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::array<char, 2048> storage{};
            while (payloads.size() < expected && std::chrono::steady_clock::now() < deadline) {
                const ssize_t received = ::recv(peerFd, storage.data(), storage.size(), 0);
                if (received >= 0) {
                    payloads.emplace_back(storage.data(), static_cast<std::size_t>(received));
                }
                else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else {
                    break;
                }
            }
            return payloads;
        };
        const auto sendToClient = [peerFd, &clientAddress](const char* bytes) {
            const std::size_t length = std::strlen(bytes);
            return ::sendto(
                peerFd, bytes, length, 0, clientAddress.SockAddr(), clientAddress.Length())
                == static_cast<ssize_t>(length);
        };

        const auto openingFlights = receiveDatagrams(2, std::chrono::seconds(3));
        Require(openingFlights == std::vector<std::string>{
                "client-flight-1", "client-flight-retry" },
            "Real DTLS client should emit initial and timeout retransmit flights");
        Require(state.m_retransmitCallbacks.load(std::memory_order_acquire) == 1,
            "Real io_uring timeout CQE should invoke the client Engine once");
        Require(sendToClient("cookie-response"), "Raw peer should send cookie response");
        const auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (state.m_handshakeCallbacks.load(std::memory_order_acquire) != 1
            && std::chrono::steady_clock::now() < handshakeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(state.m_handshakeCallbacks.load(std::memory_order_acquire) == 1,
            "Real DTLS client cookie response should enter Active once");
        Require(receiveDatagrams(2, std::chrono::seconds(3)) == std::vector<std::string>{
                "client-flight-2a", "client-flight-2b" },
            "Real DTLS client should preserve multi-datagram handshake output");

        connection->Send("application", 11);
        Require(receiveDatagrams(2, std::chrono::seconds(3)) == std::vector<std::string>{
                "app-flight-1", "app-flight-2" },
            "Real DTLS client plaintext send should produce two ciphertext datagrams");
        Require(sendToClient("server-data"), "Raw peer should send encrypted application data");
        const auto plaintextDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state.m_mutex);
                if (state.m_plaintext.size() == 2) break;
            }
            if (std::chrono::steady_clock::now() >= plaintextDeadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> lock(state.m_mutex);
            Require(state.m_plaintext == std::vector<std::string>{ "plain-one", "plain-two" },
                "Real DTLS client should deliver each plaintext batch element separately");
            Require(state.m_plaintextPeers == std::vector<std::string>{
                    peerAddress.ToString(), peerAddress.ToString() },
                "Real DTLS client plaintext callbacks should expose the exact connected peer");
        }

        connection->Shutdown();
        Require(receiveDatagrams(1, std::chrono::seconds(3))
                == std::vector<std::string>{ "client-close" },
            "Real DTLS client graceful shutdown should send its close flight");
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!state.m_connectionClosed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!state.m_connectionClosed.load(std::memory_order_acquire)) connection->ForceClose();
        loop.Shutdown();
        worker.join();
        (void)::close(peerFd);
        Require(state.m_connectionClosed.load(std::memory_order_acquire)
                && state.m_sessionClosedCallbacks.load(std::memory_order_acquire) == 1
                && state.m_lastError.load(std::memory_order_acquire) == 0,
            "Real DTLS client close CQE should finalize session and Connection exactly once");
        const auto stats = loop.GetCompletionStats(); // 真实 UDP completion 证据
        Require(stats.datagramReceiveCompletions >= 2 && stats.datagramSendCompletions >= 7,
            "Real DTLS client should execute both receive and send completion paths");
#endif
    }

    // 真实 io_uring unconnected Server 覆盖两 peer 隔离、重传、duplicate、linked batch 与独立关闭。
    void TestRealDtlsUnconnectedServerDataPath() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING) && !defined(_WIN32)
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 内核分配 server 端口
        const int serverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
        Require(serverFd >= 0 && ::bind(serverFd, any.SockAddr(), any.Length()) == 0,
            "Real DTLS server socket should bind");
        Require(SetTestNonBlocking(serverFd), "Real DTLS server socket should be nonblocking");
        const LikesProgram::Net::Address serverAddress =
            LikesProgram::Net::Address::GetLocalAddress(serverFd); // 两个 raw client 固定目标

        const auto openClient = [&serverAddress]() {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 独立 peer flow
            if (fd < 0 || ::connect(fd, serverAddress.SockAddr(), serverAddress.Length()) != 0
                || !SetTestNonBlocking(fd)) {
                if (fd >= 0) (void)::close(fd);
                return -1;
            }
            return fd;
        };
        const int clientAFd = openClient();
        const int clientBFd = openClient();
        Require(clientAFd >= 0 && clientBFd >= 0, "Real DTLS raw clients should connect");
        const LikesProgram::Net::Address peerA = LikesProgram::Net::Address::GetLocalAddress(clientAFd);
        const LikesProgram::Net::Address peerB = LikesProgram::Net::Address::GetLocalAddress(clientBFd);

        LikesProgram::Net::EventLoop loop; // 真实 io_uring unconnected UDP issuer
        DtlsServerRoutingState state;
        auto connection = std::make_shared<DtlsServerRoutingConnection>(serverFd, &loop, state);
        connection->SetDtlsSessionIdleTimeout(std::chrono::milliseconds(0));
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto sendPayload = [](int fd, const char* bytes) {
            const std::size_t length = std::strlen(bytes);
            return ::send(fd, bytes, length, 0) == static_cast<ssize_t>(length);
        };
        const auto receiveDatagrams = [](int fd, std::size_t expected) {
            std::vector<std::string> payloads; // 当前 raw peer 收到的完整 server flights
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            std::array<char, 2048> storage{};
            while (payloads.size() < expected && std::chrono::steady_clock::now() < deadline) {
                const ssize_t received = ::recv(fd, storage.data(), storage.size(), 0);
                if (received >= 0) {
                    payloads.emplace_back(storage.data(), static_cast<std::size_t>(received));
                }
                else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else {
                    break;
                }
            }
            return payloads;
        };

        Require(sendPayload(clientAFd, "valid-cookie") && sendPayload(clientBFd, "valid-cookie"),
            "Real DTLS clients should send interleaved handshakes");
        Require(receiveDatagrams(clientAFd, 1).size() == 1
                && receiveDatagrams(clientBFd, 1).size() == 1,
            "Real DTLS server should return one Active flight per peer");
        const auto activeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (connection->GetDtlsSessionStats().activeSessions != 2
            && std::chrono::steady_clock::now() < activeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(connection->GetDtlsSessionStats().activeSessions == 2,
            "Real DTLS server should publish two Active peer sessions");

        Require(sendPayload(clientAFd, "arm"), "Peer A should arm a retransmit timer");
        const auto retransmit = receiveDatagrams(clientAFd, 1);
        Require(retransmit.size() == 1
                && retransmit.front().rfind("retransmit:", 0) == 0,
            "Real DTLS server timer CQE should send peer A retransmit flight");
        Require(sendPayload(clientBFd, "data:B-real")
                && sendPayload(clientBFd, "data:B-real"),
            "Peer B should send one record and one duplicate");
        Require(sendPayload(clientBFd, "multi-flight"),
            "Peer B should request a linked multi-datagram flight");
        Require(receiveDatagrams(clientBFd, 4).size() == 4,
            "Real DTLS server should preserve all four linked ciphertext datagrams");

        Require(sendPayload(clientAFd, "close-flight"), "Peer A should request graceful close");
        Require(receiveDatagrams(clientAFd, 1) == std::vector<std::string>{ "close-flight" },
            "Peer A should receive its close flight before session removal");
        const auto peerCloseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (connection->GetDtlsSessionStats().activeSessions != 1
            && std::chrono::steady_clock::now() < peerCloseDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(connection->GetDtlsSessionStats().activeSessions == 1
                && connection->GetState() == LikesProgram::Net::Connection::State::Connected,
            "Real peer A close should preserve peer B and shared server socket");

        Require(sendPayload(clientBFd, "data:B-after"),
            "Peer B should continue traffic after peer A closes");
        const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (state.m_totalPlaintextCallbacks.load(std::memory_order_acquire) < 2
            && std::chrono::steady_clock::now() < deliveryDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto completionStats = loop.GetCompletionStats(); // ForceClose 前的真实快路径证据
        connection->ForceClose();
        const auto connectionCloseDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < connectionCloseDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();
        (void)::close(clientAFd);
        (void)::close(clientBFd);

        Require(state.m_handshakeCallbacks[peerA.ToString()] == 1
                && state.m_handshakeCallbacks[peerB.ToString()] == 1,
            "Real DTLS server should isolate both handshake callbacks");
        Require(state.m_plaintextByPeer[peerB.ToString()] == "B-after"
                && state.m_plaintextCallbacks[peerB.ToString()] == 2,
            "Real DTLS server should suppress duplicate B record and accept later B traffic");
        Require(state.m_closedCallbacks[peerA.ToString()] == 1,
            "Real peer A close CQE should notify exactly once before server shutdown");
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // 公共 DTLS 语义不依赖 io_uring 能力
        if (epollBackend) {
            Require(!completionStats.datagramMultishotEnabled
                    && completionStats.datagramProvidedBufferCount == 0
                    && completionStats.datagramMultishotReceiveCompletions == 0
                    && completionStats.datagramActiveBufferLeases == 0
                    && !completionStats.receiveBundleEnabled
                    && completionStats.maximumDatagramSendBatch <= 1,
                "Epoll DTLS should report one-shot datagram capabilities");
        }
        else {
            Require(completionStats.datagramMultishotEnabled
                    && completionStats.datagramMultishotReceiveCompletions >= 8
                    && completionStats.maximumDatagramSendBatch > 1,
                "Real DTLS server should execute multishot receive and linked send fast paths");
        }
#endif
    }

    // fake Poller 必须逐项保留 payload、peer、原始长度与截断状态。
    void TestDatagramPollerBridgePreservesMetadata() {
#ifndef _WIN32
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 有效 fd 允许 Connection 进入 direct Poller
        Require(fd >= 0, "Datagram bridge socket should open");

        auto poller = std::make_unique<OwnershipPoller>(); // 捕获 SendTo 的显式 peer 和 payload
        OwnershipPoller* pollerAddress = poller.get(); // EventLoop 接管后的观察地址
        LikesProgram::Net::EventLoop loop(std::move(poller));
        auto connection = std::make_shared<DatagramCaptureConnection>(
            fd,
            &loop,
            LikesProgram::Net::TransportKind::Udp);
        loop.AttachConnection(connection);
        const int channelCountBeforeStart = pollerAddress->ChannelAddCount(); // 只允许 wakeup Channel
        connection->Start();

        LikesProgram::Net::Buffer input; // 模拟 recvmsg 容量内 payload
        input.Append("ping", 4);
        const LikesProgram::Net::Address peer("127.0.0.1", 9000);
        LikesProgram::Net::Internal::PollerAccess::CompleteDatagram(
            *connection,
            input,
            peer,
            9,
            true);
        Require(connection->m_called && connection->m_payload == "ping",
            "Datagram bridge should deliver the callback-scoped payload");
        Require(connection->m_peer.ToString() == peer.ToString() && connection->m_originalBytes == 9
                && connection->m_truncated,
            "Datagram bridge should preserve peer, wire length, and truncation");

        connection->SendTo(peer, "pong", 4);
        connection->ForceClose();
        loop.Shutdown();
        loop.Start(); // 顺序处理 SendTo 与关闭任务
        Require(pollerAddress->DatagramPeer().ToString() == peer.ToString()
                && pollerAddress->DatagramWrite().AsStringView() == "pong",
            "SendTo should reach the Poller with its explicit peer and payload");
        Require(pollerAddress->ChannelAddCount() == channelCountBeforeStart,
            "Unconnected UDP should not create a Channel-backed I/O path");
#endif
    }

    void TestCompletionBackpressureControlsReadAndClose() {
        auto poller = std::make_unique<OwnershipPoller>(); // 同步 fake CQE 避免依赖具体内核时序
        OwnershipPoller* observer = poller.get(); // EventLoop 生命周期内观察 completion 状态
        LikesProgram::Net::EventLoop loop(std::move(poller));
        int sockets[2] = { -1, -1 }; // 有效 fd 让 Connection 进入 direct Poller I/O
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Completion backpressure socketpair should open");

        auto connection = std::make_shared<CompletionBackpressureConnection>(
            sockets[0],
            &loop,
            *observer);
        connection->SetWriteWatermark(4, 2);
        loop.AttachConnection(connection);
        connection->Start(); // OnConnected 内同步完成 High/Low/Overflow 全链

        Require(connection->ContractFinished(),
            "Completion backpressure contract should finish inside the issuer callback");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Completion overflow should leave the connection closed");
        (void)::close(sockets[1]);
    }

    void TestCompletionShutdownRejectsLateWrites() {
        auto poller = std::make_unique<OwnershipPoller>(); // 同步完成项精确控制排空顺序
        OwnershipPoller* observer = poller.get(); // EventLoop 生命周期内非拥有观察指针
        LikesProgram::Net::EventLoop loop(std::move(poller));
        int sockets[2] = { -1, -1 }; // ShutdownWrite 和最终 close 使用真实 stream fd
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Completion shutdown socketpair should open");

        auto connection = std::make_shared<CompletionShutdownConnection>(
            sockets[0],
            &loop,
            *observer);
        loop.AttachConnection(connection);
        connection->Start(); // OnConnected 内同步完成 Closing、drain 与 peer EOF

        Require(connection->ContractFinished(),
            "Completion graceful shutdown contract should finish in the issuer callback");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Completion graceful shutdown should finish closed");
        (void)::close(sockets[1]);
    }

    void TestRealCompletionPartialSendDrainsBeforeShutdown() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 真实 io_uring send/cancel/read completion 驱动状态机
        int sockets[2] = { -1, -1 }; // peer 延迟读取以稳定制造 partial send
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Real completion backpressure socketpair should open");
        Require(SetTestNonBlocking(sockets[0]) && SetTestNonBlocking(sockets[1]),
            "Real completion backpressure sockets should enter nonblocking mode");
        int sendBufferBytes = 4 * 1024; // 小内核发送缓冲保证 1 MiB 写无法单次完成
        Require(::setsockopt(
                sockets[0],
                SOL_SOCKET,
                SO_SNDBUF,
                &sendBufferBytes,
                sizeof(sendBufferBytes)) == 0,
            "Real completion backpressure should constrain SO_SNDBUF");

        RealBackpressureState state; // issuer 回调通过原子状态向测试线程发布
        auto connection = std::make_shared<RealBackpressureConnection>(sockets[0], &loop, state);
        connection->SetWriteWatermark(128 * 1024, 64 * 1024);
        connection->SetMaxPendingWriteBytes(2 * 1024 * 1024);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto connectedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state.m_connected.load(std::memory_order_acquire) == 0
            && std::chrono::steady_clock::now() < connectedDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool connected = state.m_connected.load(std::memory_order_acquire) == 1; // 延迟统一断言并清理线程

        std::string expected(1024 * 1024, '\0'); // 大于受限 SO_SNDBUF，必须经过多次 send CQE
        for (std::size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<char>((index * 29 + 7) & 0xff);
        }
        if (connected) {
            LikesProgram::Net::Buffer payload(expected.size()); // 所有权直接移入 Poller 写链
            payload.Append(expected.data(), expected.size());
            connection->Send(std::move(payload));
            connection->Shutdown(); // Send 已登记，关闭必须等待整条写链排空

            LikesProgram::Net::Buffer rejected; // Closing 发布后不得进入真实 socket 写链
            rejected.Append("late", 4);
            connection->Send(std::move(rejected));
        }

        const auto highDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state.m_highCount.load(std::memory_order_acquire) == 0
            && std::chrono::steady_clock::now() < highDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool highObserved = state.m_highCount.load(std::memory_order_acquire) == 1; // peer 此前不读取

        std::string actual; // 持续读取到 graceful write shutdown 产生 EOF
        actual.reserve(expected.size());
        bool peerEof = false; // 只有写链排空并执行 shutdown(SHUT_WR) 后才能为 true
        std::array<char, 64 * 1024> receiveBuffer{}; // peer 单次 drain 上界
        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!peerEof && std::chrono::steady_clock::now() < drainDeadline) {
            const ssize_t received = ::recv(
                sockets[1],
                receiveBuffer.data(),
                receiveBuffer.size(),
                0);
            if (received > 0) {
                actual.append(receiveBuffer.data(), static_cast<std::size_t>(received));
                continue;
            }
            if (received == 0) {
                peerEof = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        (void)::close(sockets[1]); // peer close 让恢复后的 multishot read 收到最终 EOF
        sockets[1] = -1;
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();

        Require(connected, "Real completion backpressure connection should start");
        Require(highObserved && state.m_highCount.load(std::memory_order_acquire) == 1,
            "Real completion write should cross High exactly once before peer drain");
        Require(state.m_lowCount.load(std::memory_order_acquire) == 1,
            "Partial send CQEs should cross Low exactly once while peer drains");
        Require(state.m_writeCompleteCount.load(std::memory_order_acquire) == 1,
            "Real completion write chain should report one final completion");
        Require(peerEof,
            "Graceful shutdown should half-close only after every accepted byte is sent");
        Require(actual == expected,
            "Graceful shutdown should preserve payload order and reject the late write");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed
                && state.m_closedCount.load(std::memory_order_acquire) == 1,
            "Peer EOF should complete the real unified close path exactly once");
#endif
    }

    void TestTcpSmallReadKeepsBufferBounded() {
#ifndef _WIN32
        int sockets[2] = { -1, -1 }; // 本地 socketpair 隔离单次 TCP transport 读取策略
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "TCP small read socketpair should open");
        Require(SetTestNonBlocking(sockets[0]),
            "TCP small read receiver should enter nonblocking mode");

        const char payload = 'x'; // 单字节负载用于验证小请求不会预留 64 KiB
        Require(::send(sockets[1], &payload, 1, 0) == 1,
            "TCP small read sender should write one byte");

        LikesProgram::Net::TcpTransport receiver(
            sockets[0],
            LikesProgram::Net::TcpUpgradeMode::Disabled);
        LikesProgram::Net::Buffer received;
        const LikesProgram::Net::IoResult result = receiver.ReadSome(received); // 直接读取真实 socket

        Require(result.status == LikesProgram::Net::IoStatus::Ok && result.nbytes == 1,
            "TCP small read should receive one byte");
        Require(received.AsStringView() == "x", "TCP small read payload mismatch");
        Require(received.WritableBytes() <= 16 * 1024,
            "TCP small read should not retain a 64 KiB receive allocation");
        (void)::close(sockets[1]);
#endif
    }

    void TestAddress() {
        LikesProgram::Net::Address address("127.0.0.1", 0);

        Require(address.IsValid(), "IPv4 loopback address should be valid");
        Require(address.Ip() == "127.0.0.1", "Address IP mismatch");
        Require(address.Port() == 0, "Address port mismatch");
        Require(!address.ToString().empty(), "Address ToString should not be empty");
    }

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    void TestUserSecureExtensionPoint() {
        UserSecureTcpTransport tcpTransport(LikesProgram::Net::kInvalidSocket);
        UserSecureUdpTransport udpTransport(LikesProgram::Net::kInvalidSocket);
        const std::uint8_t data[] = { 'o', 'k' };

        Require(tcpTransport.Kind() == LikesProgram::Net::TransportKind::Tcp,
            "UserSecureTcpTransport kind mismatch");
        Require(udpTransport.Kind() == LikesProgram::Net::TransportKind::Udp,
            "UserSecureUdpTransport kind mismatch");
        Require(tcpTransport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureTcpTransport secure init mismatch");
        Require(tcpTransport.Initialized(), "UserSecureTcpTransport init flag mismatch");
        Require(tcpTransport.SecurityState() == LikesProgram::Net::TransportSecurityState::Plain,
            "InitializeSecureLayer should not enter handshake state");
        Require(tcpTransport.WriteSome(data, sizeof(data)).nbytes == 2,
            "UserSecureTcpTransport plain write mismatch");
        Require(tcpTransport.SocketWriteUsed(), "UserSecureTcpTransport should keep socket write by default");
        Require(!tcpTransport.SecureWriteUsed(), "UserSecureTcpTransport should not use secure write before upgrade");

        Require(udpTransport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureUdpTransport upgrade result mismatch");
        Require(udpTransport.Upgraded(), "UserSecureUdpTransport upgrade flag mismatch");
        Require(udpTransport.NeedHandshake(), "UserSecureUdpTransport should enter handshake state after upgrade");
        Require(udpTransport.Handshake().status == LikesProgram::Net::IoStatus::Ok,
            "UserSecureUdpTransport handshake result mismatch");
        Require(udpTransport.HandshakeCompleted(), "UserSecureUdpTransport handshake callback mismatch");
        Require(udpTransport.SecurityState() == LikesProgram::Net::TransportSecurityState::Secure,
            "UserSecureUdpTransport secure state mismatch");
        Require(udpTransport.WriteSome(data, sizeof(data)).nbytes == 2,
            "UserSecureUdpTransport secure write mismatch");
        Require(udpTransport.SecureWriteUsed(), "UserSecureUdpTransport should use secure write after handshake");
    }

    void TestDelayedUpgradeStateMachine() {
        LikesProgram::Net::TcpTransport disabledTransport(
            LikesProgram::Net::kInvalidSocket,
            LikesProgram::Net::TcpUpgradeMode::Disabled);
        Require(disabledTransport.UpgradeMode() == LikesProgram::Net::TcpUpgradeMode::Disabled,
            "Disabled transport mode should be visible");
        Require(!disabledTransport.NeedHandshake(), "Disabled transport should skip handshake checks");
        Require(disabledTransport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "Disabled transport keeps upgrade request as no-op");

        UserSecureTcpTransport transport(LikesProgram::Net::kInvalidSocket);
        const std::uint8_t data[] = { 'p', 'l', 'a', 'i', 'n' };

        Require(transport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade init should succeed");
        Require(transport.SecurityState() == LikesProgram::Net::TransportSecurityState::Plain,
            "Delayed upgrade should start in plain state");
        Require(transport.WriteSome(data, sizeof(data)).status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade plain write should use socket fallback");
        Require(transport.SocketWriteUsed(), "Delayed upgrade should keep socket ability before upgrade");

        Require(transport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade request should succeed");
        Require(transport.NeedHandshake(), "Delayed upgrade should enter handshake state");
        Require(transport.Handshake().status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade handshake should succeed");
        Require(transport.SecurityState() == LikesProgram::Net::TransportSecurityState::Secure,
            "Delayed upgrade should enter secure state after handshake");
        Require(transport.WriteSome(data, sizeof(data)).status == LikesProgram::Net::IoStatus::Ok,
            "Delayed upgrade secure write should succeed");
        Require(transport.SecureWriteUsed(), "Delayed upgrade should use secure hook after handshake");

        auto sharedContext = std::make_shared<StartTlsLikeTcpTransport::SharedSecureContext>();
        StartTlsLikeTcpTransport immediateTransport(
            LikesProgram::Net::kInvalidSocket,
            sharedContext,
            LikesProgram::Net::TcpUpgradeMode::Immediate);
        Require(immediateTransport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "Immediate upgrade init should enter upgrade path");
        Require(immediateTransport.UpgradeCount() == 1,
            "Immediate upgrade mode should request upgrade during initialization");
        Require(immediateTransport.NeedHandshake(),
            "Immediate upgrade mode should enter handshake state");
        Require(immediateTransport.Handshake().status == LikesProgram::Net::IoStatus::Ok,
            "Immediate upgrade handshake should succeed");
        Require(immediateTransport.SecurityState() == LikesProgram::Net::TransportSecurityState::Secure,
            "Immediate upgrade should enter secure state after handshake");

        StartTlsLikeTcpTransport autoStartTlsTransport(
            LikesProgram::Net::kInvalidSocket,
            sharedContext,
            LikesProgram::Net::TcpUpgradeMode::Auto);
        Require(autoStartTlsTransport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "Auto STARTTLS init should succeed");
        Require(autoStartTlsTransport.UpgradeCount() == 0,
            "Auto STARTTLS mode should wait for protocol command by default");
        Require(!autoStartTlsTransport.NeedHandshake(),
            "Auto STARTTLS mode should remain plain before command or TLS first packet");
        Require(autoStartTlsTransport.UpgradeCommunication().status == LikesProgram::Net::IoStatus::Ok,
            "Auto STARTTLS manual upgrade should still be accepted");
        Require(autoStartTlsTransport.NeedHandshake(),
            "Auto STARTTLS manual upgrade should enter handshake state");

        StartTlsLikeTcpTransport autoImmediateTransport(
            LikesProgram::Net::kInvalidSocket,
            sharedContext,
            LikesProgram::Net::TcpUpgradeMode::Auto,
            true);
        Require(autoImmediateTransport.InitializeSecureLayer().status == LikesProgram::Net::IoStatus::Ok,
            "Auto immediate init should succeed");
        Require(autoImmediateTransport.UpgradeCount() == 1,
            "Auto immediate mode should request upgrade during initialization");

        const std::uint8_t tlsClientHelloPrefix[] = { 0x16, 0x03, 0x01, 0x00, 0x2a };
        const std::uint8_t startTlsCommandPrefix[] = { 'S', 'T', 'A', 'R', 'T' };
        Require(autoImmediateTransport.DetectsAutoUpgradePacket(
            tlsClientHelloPrefix,
            sizeof(tlsClientHelloPrefix)),
            "Auto mode should recognize TLS record headers");
        Require(!autoImmediateTransport.DetectsAutoUpgradePacket(
            startTlsCommandPrefix,
            sizeof(startTlsCommandPrefix)),
            "Auto mode should keep STARTTLS commands on the plain path");
    }

#endif

    void TestUdpSendToExplicitPeer() {
        LikesProgram::Net::Address any("127.0.0.1", 0); // 构造地址时确保平台 socket runtime 已初始化

        LikesProgram::Net::SocketType receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(receiverFd != LikesProgram::Net::kInvalidSocket, "UDP receiver socket should open");
        LikesProgram::Net::UdpTransport receiver(receiverFd);
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0, "UDP receiver bind should succeed");
        Require(SetTestNonBlocking(receiverFd), "UDP receiver should enter nonblocking mode");

        LikesProgram::Net::Address receiverAddress = LikesProgram::Net::Address::GetLocalAddress(receiverFd);
        Require(receiverAddress.IsValid() && receiverAddress.Port() != 0, "UDP receiver address should be bound");

        LikesProgram::Net::SocketType senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(senderFd != LikesProgram::Net::kInvalidSocket, "UDP sender socket should open");
        LikesProgram::Net::UdpTransport sender(senderFd);
        Require(SetTestNonBlocking(senderFd), "UDP sender should enter nonblocking mode");

        const std::uint8_t payload[] = { 'p', 'e', 'e', 'r' };
        Require(sender.SendTo(LikesProgram::Net::Address(), payload, sizeof(payload)).status
            == LikesProgram::Net::IoStatus::Error,
            "UDP SendTo should reject invalid peer");

        const auto sent = sender.SendTo(receiverAddress, payload, sizeof(payload));
        Require(sent.status == LikesProgram::Net::IoStatus::Ok && sent.nbytes == 4,
            "UDP SendTo should send payload to explicit peer");

        LikesProgram::Net::Buffer received;
        LikesProgram::Net::IoResult read{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            read = receiver.ReadSome(received);
            if (read.status == LikesProgram::Net::IoStatus::Ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (read.status == LikesProgram::Net::IoStatus::WouldBlock
            && std::chrono::steady_clock::now() < deadline);

        Require(read.status == LikesProgram::Net::IoStatus::Ok, "UDP receiver should read explicit SendTo datagram");
        Require(received.AsStringView() == "peer", "UDP SendTo payload mismatch");
        Require(receiver.HasLastPeer(), "UDP receiver should remember sender peer");
        Require(receiver.LastPeerAddress().IsValid(), "UDP last peer address should be valid");
    }

#if defined(__linux__)
    void TestTcpWriteToClosedPeerDoesNotRaiseSigpipe() {
        int sockets[2]{ -1, -1 }; // 子进程使用的本地 TCP 语义 socketpair
        int readyPipe[2]{ -1, -1 }; // 父进程关闭 peer 后释放子进程发送
        Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "Linux SIGPIPE test socketpair should open");
        Require(::pipe(readyPipe) == 0, "Linux SIGPIPE synchronization pipe should open");

        const pid_t child = ::fork(); // 独立进程隔离默认 SIGPIPE 终止行为
        Require(child >= 0, "Linux SIGPIPE test should fork");
        if (child == 0) {
            (void)::close(sockets[1]);
            (void)::close(readyPipe[1]);

            char signal = 0; // 读取 EOF 表示父进程已经关闭全部 peer 副本
            const ssize_t readyResult =
                ::read(readyPipe[0], &signal, sizeof(signal)); // 父进程关闭写端后必须返回 EOF
            (void)::close(readyPipe[0]);
            if (readyResult != 0) ::_exit(3);

            LikesProgram::Net::TcpTransport transport(
                sockets[0],
                LikesProgram::Net::TcpUpgradeMode::Disabled);
            const std::uint8_t payload = 1; // 触发已关闭 peer 的真实 send 路径
            const LikesProgram::Net::IoResult result = transport.WriteSome(&payload, sizeof(payload));
            ::_exit(result.status == LikesProgram::Net::IoStatus::Error ? 0 : 2);
        }

        (void)::close(sockets[0]);
        (void)::close(sockets[1]);
        (void)::close(readyPipe[0]);
        (void)::close(readyPipe[1]); // 关闭写端后，子进程才开始 send

        int status = 0; // waitpid 返回的子进程退出状态
        Require(::waitpid(child, &status, 0) == child, "Linux SIGPIPE child should be reaped");
        Require(WIFEXITED(status), "Closed-peer send should not terminate process with SIGPIPE");
        Require(WEXITSTATUS(status) == 0, "Closed-peer send should return IoStatus::Error");
    }
#endif

    void TestUdpBatchSendReceive() {
        LikesProgram::Net::Address any("127.0.0.1", 0); // 使用本地随机端口，避免测试间端口冲突。

        LikesProgram::Net::SocketType receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(receiverFd != LikesProgram::Net::kInvalidSocket, "UDP batch receiver socket should open");
        LikesProgram::Net::UdpTransport receiver(receiverFd);
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0, "UDP batch receiver bind should succeed");
        Require(SetTestNonBlocking(receiverFd), "UDP batch receiver should enter nonblocking mode");

        LikesProgram::Net::Address receiverAddress = LikesProgram::Net::Address::GetLocalAddress(receiverFd);
        Require(receiverAddress.IsValid() && receiverAddress.Port() != 0, "UDP batch receiver address should be bound");

        LikesProgram::Net::SocketType senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Require(senderFd != LikesProgram::Net::kInvalidSocket, "UDP batch sender socket should open");
        LikesProgram::Net::UdpTransport sender(senderFd);
        Require(SetTestNonBlocking(senderFd), "UDP batch sender should enter nonblocking mode");

        const std::uint8_t first[] = { 'b', '1' };
        const std::uint8_t second[] = { 'b', '2' };
        const std::uint8_t third[] = { 'b', '3' };
        LikesProgram::Net::Address invalidPeer;
        LikesProgram::Net::UdpSendDatagram invalidSend{};
        invalidSend.peer = &invalidPeer;
        invalidSend.data = first;
        invalidSend.len = sizeof(first);
        Require(sender.SendBatch(&invalidSend, 1).status == LikesProgram::Net::IoStatus::Error,
            "UDP SendBatch should reject invalid peer");

        std::array<LikesProgram::Net::UdpSendDatagram, 3> sends{};
        sends[0].peer = &receiverAddress;
        sends[0].data = first;
        sends[0].len = sizeof(first);
        sends[1].peer = &receiverAddress;
        sends[1].data = second;
        sends[1].len = sizeof(second);
        sends[2].peer = &receiverAddress;
        sends[2].data = third;
        sends[2].len = sizeof(third);

        const auto sent = sender.SendBatch(sends.data(), sends.size());
        Require(sent.status == LikesProgram::Net::IoStatus::Ok && sent.nbytes == 6,
            "UDP SendBatch should send all payload bytes");
        for (const auto& item : sends) {
            Require(item.result.status == LikesProgram::Net::IoStatus::Ok,
                "UDP SendBatch item should be marked successful");
        }

        LikesProgram::Net::UdpReceiveDatagram invalidReceive{};
        Require(receiver.ReadBatch(&invalidReceive, 1).status == LikesProgram::Net::IoStatus::Error,
            "UDP ReadBatch should reject null buffer");

        LikesProgram::Net::Buffer firstBuffer;
        LikesProgram::Net::Buffer secondBuffer;
        LikesProgram::Net::Buffer thirdBuffer;
        LikesProgram::Net::Address firstPeer;
        LikesProgram::Net::Address secondPeer;
        LikesProgram::Net::Address thirdPeer;
        std::array<LikesProgram::Net::UdpReceiveDatagram, 3> receives{};
        receives[0].buffer = &firstBuffer;
        receives[0].peer = &firstPeer;
        receives[1].buffer = &secondBuffer;
        receives[1].peer = &secondPeer;
        receives[2].buffer = &thirdBuffer;
        receives[2].peer = &thirdPeer;

        std::size_t receivedCount = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (receivedCount < receives.size() && std::chrono::steady_clock::now() < deadline) {
            const auto read = receiver.ReadBatch(
                receives.data() + receivedCount,
                receives.size() - receivedCount);
            if (read.status == LikesProgram::Net::IoStatus::WouldBlock) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            Require(read.status == LikesProgram::Net::IoStatus::Ok,
                "UDP ReadBatch should read batch datagrams");
            while (receivedCount < receives.size()
                && receives[receivedCount].result.status == LikesProgram::Net::IoStatus::Ok) {
                ++receivedCount;
            }
        }

        Require(receivedCount == receives.size(), "UDP ReadBatch should receive all datagrams");
        Require(firstBuffer.AsStringView() == "b1", "UDP ReadBatch first payload mismatch");
        Require(secondBuffer.AsStringView() == "b2", "UDP ReadBatch second payload mismatch");
        Require(thirdBuffer.AsStringView() == "b3", "UDP ReadBatch third payload mismatch");
        Require(firstPeer.IsValid() && secondPeer.IsValid() && thirdPeer.IsValid(),
            "UDP ReadBatch should expose sender peers");
        Require(receiver.HasLastPeer(), "UDP ReadBatch should update last peer");
    }

    void TestRoundTrip(
        LikesProgram::Net::TransportKind kind,
        const char* label,
        std::size_t workerThreads = 0) {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            kind,
            [kind](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop, kind);
            });

        server.SetWorkerThreads(workerThreads);
        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Server should expose bound address");
        Require(listenAddresses.front().Port() != 0, "Server should expose assigned port");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            kind,
            [&received, &payload, kind](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload,
                    kind);
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Client should create connection");
        connection->Send("ping", 4);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), label);
        Require(payload == "ping", "echo payload mismatch");
    }

    // 真实 io_uring UDP multishot 必须用少量 read SQE 交付完整有序数据报。
    void TestUdpMultishotReceiveBatch() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 单 issuer 真实 io_uring UDP completion
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 内核分配独立回环端口
        const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管接收 socket
        Require(receiverFd >= 0, "UDP multishot receiver socket should open");
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
            "UDP multishot receiver should bind a loopback address");
        Require(SetTestNonBlocking(receiverFd),
            "UDP multishot receiver should enter nonblocking mode");
        const LikesProgram::Net::Address receiverAddress =
            LikesProgram::Net::Address::GetLocalAddress(receiverFd); // 预装载发送目标
        Require(receiverAddress.IsValid() && receiverAddress.Port() != 0,
            "UDP multishot receiver should expose its assigned port");

        const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 同一 flow 保证顺序口径
        Require(senderFd >= 0, "UDP multishot sender socket should open");
        for (std::uint32_t sequence = 0; sequence < 64; ++sequence) {
            const ssize_t sent = ::sendto(
                senderFd,
                &sequence,
                sizeof(sequence),
                0,
                receiverAddress.SockAddr(),
                receiverAddress.Length());
            Require(sent == static_cast<ssize_t>(sizeof(sequence)),
                "UDP multishot should preload every sequence datagram");
        }

        std::vector<std::uint32_t> sequences; // worker 结束后由测试线程验证顺序
        sequences.reserve(64);
        std::atomic<std::size_t> received{ 0 }; // 防止失败路径永久等待 EventLoop
        std::atomic<bool> metadataValid{ true }; // 聚合 peer、边界与截断结果
        auto connection = std::make_shared<DatagramSequenceConnection>(
            receiverFd,
            &loop,
            sequences,
            received,
            metadataValid);
        loop.AttachConnection(connection);
        const std::uint64_t submissionsBefore = loop.CompletionReadSubmissionCount(); // 首次 Poll 前基线
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (received.load(std::memory_order_acquire) < 64
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (received.load(std::memory_order_acquire) < 64) loop.Shutdown();
        worker.join();
        (void)::close(senderFd);

        Require(sequences.size() == 64, "UDP multishot should deliver every queued datagram");
        for (std::uint32_t index = 0; index < 64; ++index) {
            Require(sequences[index] == index, "UDP multishot should preserve datagram order");
        }
        Require(metadataValid.load(std::memory_order_acquire),
            "UDP multishot should preserve peer and datagram boundaries");
        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 快路径命中证据
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // epoll level drain 保留相同边界与 FIFO
        if (epollBackend) {
            Require(!stats.datagramMultishotEnabled
                    && stats.datagramProvidedBufferCount == 0
                    && stats.datagramMultishotReceiveCompletions == 0
                    && stats.datagramActiveBufferLeases == 0,
                "Epoll UDP receive should report one-shot owning-buffer capabilities");
        }
        else {
            Require(stats.datagramMultishotEnabled,
                "Both fixed Linux validation kernels should enable UDP multishot");
            Require(stats.datagramMultishotReceiveCompletions >= 64,
                "UDP receive CQEs should be attributed to the multishot path");
        }
        Require(loop.CompletionReadSubmissionCount() - submissionsBefore < 8,
            "Sixty-four datagrams should not require one read SQE per datagram");
#endif
    }

    // UDP pool 全部 lease 被业务保留时不得反复提交失败 recvmsg；归还一个 token 后必须恢复。
    void TestUdpEnobufsWaitsForReturnedToken() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::EventLoop loop; // 单 issuer 真实 io_uring UDP pool
        const LikesProgram::Net::CompletionStats initialStats =
            loop.GetCompletionStats(); // 构造后即可读取固定 UDP pool 规格
        if (IsEpollBackend(loop.CompletionBackendName())) {
            Require(initialStats.datagramProvidedBufferCount == 0
                    && initialStats.datagramProvidedBufferSize == 0
                    && initialStats.datagramActiveBufferLeases == 0
                    && initialStats.datagramPendingBufferReturns == 0
                    && initialStats.datagramCurrentAvailableBuffers == 0,
                "Epoll UDP should not expose a provided-buffer exhaustion state");
            return;
        }
        const std::size_t bufferCount = initialStats.datagramProvidedBufferCount; // 耗尽目标
        Require(bufferCount >= 64,
            "UDP ENOBUFS regression requires the registered datagram buffer pool");
        Require(initialStats.datagramActiveBufferLeases == 0
                && initialStats.datagramPendingBufferReturns == 0
                && initialStats.datagramCurrentAvailableBuffers == bufferCount,
            "Fresh UDP pool diagnostics should expose every token as available");

        const LikesProgram::Net::Address any("127.0.0.1", 0); // 独立回环接收端口
        const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
        Require(receiverFd >= 0, "UDP ENOBUFS receiver socket should open");
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
            "UDP ENOBUFS receiver should bind a loopback address");
        Require(SetTestNonBlocking(receiverFd),
            "UDP ENOBUFS receiver should enter nonblocking mode");
        const LikesProgram::Net::Address receiverAddress =
            LikesProgram::Net::Address::GetLocalAddress(receiverFd); // 原始 sender 目标

        const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程独占
        Require(senderFd >= 0, "UDP ENOBUFS sender socket should open");
        std::vector<LikesProgram::Net::Buffer> retained; // 业务故意持有全部 pool token
        retained.reserve(bufferCount + 1);
        std::atomic<std::size_t> received{ 0 }; // 发布耗尽点与归还后的额外交付
        auto connection = std::make_shared<DatagramPoolExhaustionConnection>(
            receiverFd,
            &loop,
            retained,
            received);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto stopWorker = [&]() {
            connection->ForceClose(); // 任何 RED/GREEN 失败都先收敛真实 read operation
            loop.Shutdown();
            if (worker.joinable()) worker.join();
            (void)::close(senderFd);
            retained.clear(); // Poller 仍存活时释放全部测试 lease
        };

        const auto startDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (loop.CompletionReadSubmissionCount() == 0
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (loop.CompletionReadSubmissionCount() == 0) {
            stopWorker();
            Require(false,
                "UDP ENOBUFS regression should submit the initial multishot receive");
        }

        for (std::size_t index = 0; index <= bufferCount; ++index) {
            const std::uint32_t sequence = static_cast<std::uint32_t>(index); // 小 payload 保持队列确定性
            const ssize_t sent = ::sendto(
                senderFd,
                &sequence,
                sizeof(sequence),
                0,
                receiverAddress.SockAddr(),
                receiverAddress.Length());
            Require(sent == static_cast<ssize_t>(sizeof(sequence)),
                "UDP ENOBUFS regression should queue every datagram");
        }

        LikesProgram::Net::CompletionStats exhaustedStats; // ENOBUFS terminal CQE 后的池快照
        const auto exhaustDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            exhaustedStats = loop.GetCompletionStats();
            if (received.load(std::memory_order_acquire) == bufferCount
                && exhaustedStats.datagramActiveBufferLeases == bufferCount
                && exhaustedStats.datagramCurrentAvailableBuffers == 0
                && exhaustedStats.enobufsCompletions > initialStats.enobufsCompletions) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < exhaustDeadline);
        const bool exhausted = received.load(std::memory_order_acquire) == bufferCount
            && exhaustedStats.datagramActiveBufferLeases == bufferCount
            && exhaustedStats.datagramPendingBufferReturns == 0
            && exhaustedStats.datagramCurrentAvailableBuffers == 0
            && exhaustedStats.enobufsCompletions > initialStats.enobufsCompletions;
        if (!exhausted) {
            stopWorker();
            Require(false,
                "Retained UDP leases should exhaust the pool and produce one terminal ENOBUFS");
        }

        const std::uint64_t stalledSubmissions =
            loop.CompletionReadSubmissionCount(); // 无 token 时不得继续增长
        constexpr std::size_t kFlushProbeCycles = 16; // 用跨轮任务保证至少经历多次 Flush
        std::atomic<std::size_t> flushProbeCycles{ 0 }; // 已完成的独立 EventLoop 轮次
        std::atomic<bool> submissionChurn{ false }; // 任一轮观察到续投即失败
        auto probe = std::make_shared<std::function<void()>>(); // 测试线程维持递归任务入口
        std::weak_ptr<std::function<void()>> weakProbe = probe; // 避免任务与自身形成引用环
        *probe = [&loop,
                     stalledSubmissions,
                     &flushProbeCycles,
                     &submissionChurn,
                     weakProbe]() {
            if (loop.CompletionReadSubmissionCount() != stalledSubmissions) {
                submissionChurn.store(true, std::memory_order_release);
            }
            const std::size_t cycle =
                flushProbeCycles.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (cycle < kFlushProbeCycles) {
                if (auto next = weakProbe.lock()) loop.PostTask(*next);
            }
        };
        loop.PostTask(*probe);
        const auto probeDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (flushProbeCycles.load(std::memory_order_acquire) < kFlushProbeCycles
            && std::chrono::steady_clock::now() < probeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool submissionsStable =
            flushProbeCycles.load(std::memory_order_acquire) == kFlushProbeCycles
            && !submissionChurn.load(std::memory_order_acquire)
            && loop.CompletionReadSubmissionCount() == stalledSubmissions;
        if (!submissionsStable) {
            stopWorker();
            Require(false,
                "UDP ENOBUFS should not churn read submissions across tokenless Flush cycles");
        }

        retained.front().RetrieveAll(); // 非 issuer 线程排队一个 token，下一次 Flush 先回填后重试
        loop.PostTask([]() { }); // 唤醒阻塞 Poll，确保 issuer 立即执行 drain-before-retry
        const auto recoveryDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (received.load(std::memory_order_acquire) < bufferCount + 1
            && std::chrono::steady_clock::now() < recoveryDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool recovered = received.load(std::memory_order_acquire) == bufferCount + 1
            && loop.CompletionReadSubmissionCount() > stalledSubmissions;
        if (!recovered) {
            stopWorker();
            Require(false,
                "Returning one UDP token should rearm receive and deliver the queued datagram");
        }

        retained.clear(); // worker 仍运行，issuer 必须把全部 token 回填当前 ring
        loop.PostTask([]() { }); // token 批量归还后显式唤醒 issuer 完成回填
        LikesProgram::Net::CompletionStats returnedStats; // 关闭前证明完整 pool 水位恢复
        const auto returnDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            returnedStats = loop.GetCompletionStats();
            if (returnedStats.datagramActiveBufferLeases == 0
                && returnedStats.datagramPendingBufferReturns == 0
                && returnedStats.datagramCurrentAvailableBuffers == bufferCount) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < returnDeadline);

        const bool poolReturned = returnedStats.datagramActiveBufferLeases == 0
            && returnedStats.datagramPendingBufferReturns == 0
            && returnedStats.datagramCurrentAvailableBuffers == bufferCount;
        connection->ForceClose();
        loop.Shutdown();
        worker.join();
        (void)::close(senderFd);
        Require(poolReturned,
            "Every UDP provided-buffer token should return before Poller shutdown");
#endif
    }

    // 真实 io_uring UDP 发送必须把 issuer 内连续排队项组成有界 linked SQE 批次。
    void TestUdpLinkedSendBatch() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 原始 peer 接收并验证数据报边界
        const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程独占
        Require(receiverFd >= 0, "UDP linked-batch receiver socket should open");
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
            "UDP linked-batch receiver should bind a loopback address");
        Require(SetTestNonBlocking(receiverFd),
            "UDP linked-batch receiver should enter nonblocking mode");
        const LikesProgram::Net::Address receiverAddress =
            LikesProgram::Net::Address::GetLocalAddress(receiverFd); // connected sender 默认 peer

        const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管所有权
        Require(senderFd >= 0, "UDP linked-batch sender socket should open");
        Require(::connect(senderFd, receiverAddress.SockAddr(), receiverAddress.Length()) == 0,
            "UDP linked-batch sender should connect to the receiver");
        Require(SetTestNonBlocking(senderFd),
            "UDP linked-batch sender should enter nonblocking mode");

        LikesProgram::Net::EventLoop loop; // issuer 在 OnConnected 内一次排队全部 payload
        std::atomic<bool> writeComplete{ false }; // 发送队列最终排空屏障
        auto connection = std::make_shared<DatagramBatchSendConnection>(
            senderFd,
            &loop,
            writeComplete);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        std::vector<std::uint32_t> sequences; // 原始 socket 按数据报逐项接收
        sequences.reserve(64);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sequences.size() < 64 && std::chrono::steady_clock::now() < deadline) {
            std::uint32_t sequence = 0; // 当前完整数据报的单调序号
            const ssize_t received = ::recv(receiverFd, &sequence, sizeof(sequence), 0);
            if (received == static_cast<ssize_t>(sizeof(sequence))) {
                sequences.push_back(sequence);
                continue;
            }
            if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto completionDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!writeComplete.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < completionDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        connection->ForceClose();
        loop.Shutdown();
        worker.join();
        (void)::close(receiverFd);

        Require(sequences.size() == 64, "UDP linked batch should deliver every queued datagram");
        for (std::uint32_t index = 0; index < 64; ++index) {
            Require(sequences[index] == index, "UDP linked batch should preserve FIFO sequence");
        }
        Require(writeComplete.load(std::memory_order_acquire),
            "UDP linked batch should report one final queue drain");
        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 发送快路径证据
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // epoll 逐个 syscall 仍必须保持 FIFO
        if (epollBackend) {
            Require(stats.maximumDatagramSendBatch == 1
                    && stats.datagramSendBatchSubmissions >= 64,
                "Epoll UDP should report one datagram per send syscall");
        }
        else {
            Require(stats.maximumDatagramSendBatch > 1,
                "Queued UDP sends should share a linked SQE batch");
            Require(stats.datagramSendBatchSubmissions < 64,
                "UDP batching should submit fewer batches than datagrams");
        }
        Require(stats.datagramSendCompletions >= 64 && stats.pendingDatagramSends == 0,
            "UDP linked batch stats should converge every queued datagram");
#endif
    }

    // ForceClose 必须在活动 linked 批次与长期 receive 同时存在时一次性收敛所有权。
    void TestUdpLinkedSendForceCloseWhileActive() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 接收端只提供稳定 connected peer
        const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程最终关闭
        Require(receiverFd >= 0, "UDP active-close receiver socket should open");
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
            "UDP active-close receiver should bind a loopback address");
        const LikesProgram::Net::Address receiverAddress =
            LikesProgram::Net::Address::GetLocalAddress(receiverFd); // connected sender 默认 peer

        const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
        Require(senderFd >= 0, "UDP active-close sender socket should open");
        Require(::connect(senderFd, receiverAddress.SockAddr(), receiverAddress.Length()) == 0,
            "UDP active-close sender should connect to the receiver");
        Require(SetTestNonBlocking(senderFd),
            "UDP active-close sender should enter nonblocking mode");

        InspectableCompletionEventLoop loop; // 暴露关闭 completion 只读状态
        DatagramForceCloseObservation observation; // 发送 lease 与回调收敛证据
        auto connection = std::make_shared<DatagramForceCloseConnection>(
            senderFd,
            &loop,
            observation);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto closeDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!observation.m_closed.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!observation.m_closed.load(std::memory_order_acquire)) {
            connection->ForceClose();
            loop.Shutdown();
        }
        worker.join();
        (void)::close(receiverFd);

        const std::size_t pendingAtForceClose =
            observation.m_pendingAtForceClose.load(std::memory_order_acquire); // 包含活动与尾队列
        const std::size_t batchPeakAtForceClose =
            observation.m_batchPeakAtForceClose.load(std::memory_order_acquire); // 活动 linked 批次证据
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // epoll 没有待回收 cancel CQE
        Require(observation.m_forceCloseRequested.load(std::memory_order_acquire)
                && pendingAtForceClose > 0,
            "ForceClose should run with queued datagram ownership");
        if (epollBackend) {
            Require(batchPeakAtForceClose == 1,
                "Epoll ForceClose should observe one datagram per send syscall");
        }
        else {
            Require(batchPeakAtForceClose > 1
                    && pendingAtForceClose > batchPeakAtForceClose,
                "ForceClose should run with an active linked batch and unsubmitted datagrams");
        }
        Require(observation.m_readSubmissionsAtForceClose.load(std::memory_order_acquire) > 0,
            "ForceClose should overlap an outstanding UDP receive operation");
        Require(observation.m_shutdownPendingAtClose.load(std::memory_order_acquire) != epollBackend,
            "Only io_uring ForceClose should expose pending terminal CQEs before drain");
        Require(!loop.HasPendingShutdownCompletions(),
            "EventLoop should drain every UDP shutdown completion before returning");

        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 关闭后权威快照
        Require(stats.pendingDatagramSends == 0,
            "ForceClose should converge pending datagram ownership to zero");
        Require(observation.m_callbacksAfterClose.load(std::memory_order_acquire) == 0,
            "No UDP business callback should occur after OnClosed");
        for (const auto& releaseCount : observation.m_releaseCounts) {
            Require(releaseCount.load(std::memory_order_acquire) == 1,
                "Submitted and unsubmitted UDP payload ownership should be reclaimed exactly once");
        }
#endif
    }

    // UDP graceful Shutdown 必须等待调用前已接受的全部 linked 数据报排空。
    void TestUdpLinkedSendGracefulShutdown() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        const LikesProgram::Net::Address any("127.0.0.1", 0); // 原始接收端验证关闭前完整序列
        const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程独占
        Require(receiverFd >= 0, "UDP graceful batch receiver socket should open");
        Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
            "UDP graceful batch receiver should bind a loopback address");
        Require(SetTestNonBlocking(receiverFd),
            "UDP graceful batch receiver should enter nonblocking mode");
        const LikesProgram::Net::Address receiverAddress =
            LikesProgram::Net::Address::GetLocalAddress(receiverFd); // connected sender 默认 peer

        const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
        Require(senderFd >= 0, "UDP graceful batch sender socket should open");
        Require(::connect(senderFd, receiverAddress.SockAddr(), receiverAddress.Length()) == 0,
            "UDP graceful batch sender should connect to the receiver");
        Require(SetTestNonBlocking(senderFd),
            "UDP graceful batch sender should enter nonblocking mode");

        LikesProgram::Net::EventLoop loop; // graceful close 与 CQE 回收共享 issuer
        std::atomic<bool> writeComplete{ false }; // 最后一个发送 CQE 的排空通知
        std::atomic<bool> queued{ false }; // Shutdown 前 64 个 Send 已全部登记
        auto connection = std::make_shared<DatagramBatchSendConnection>(
            senderFd,
            &loop,
            writeComplete,
            &queued);
        loop.AttachConnection(connection);
        loop.PostTask([connection]() { connection->Start(); });
        std::thread worker([&loop]() { loop.Start(); });

        const auto queueDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!queued.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < queueDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(queued.load(std::memory_order_acquire),
            "UDP graceful batch should queue every datagram before Shutdown");
        connection->Shutdown(); // API 返回后拒绝新写，但必须排空已接受队列

        std::vector<std::uint32_t> sequences; // 关闭前必须完整收到 64 个边界
        sequences.reserve(64);
        const auto receiveDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sequences.size() < 64 && std::chrono::steady_clock::now() < receiveDeadline) {
            std::uint32_t sequence = 0; // 当前数据报序号
            const ssize_t received = ::recv(receiverFd, &sequence, sizeof(sequence), 0);
            if (received == static_cast<ssize_t>(sizeof(sequence))) {
                sequences.push_back(sequence);
                continue;
            }
            if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop.Shutdown();
        worker.join();
        (void)::close(receiverFd);

        Require(sequences.size() == 64,
            "UDP graceful Shutdown should drain every accepted datagram");
        for (std::uint32_t index = 0; index < 64; ++index) {
            Require(sequences[index] == index,
                "UDP graceful Shutdown should preserve linked FIFO order");
        }
        Require(writeComplete.load(std::memory_order_acquire)
                && connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "UDP graceful Shutdown should close only after one final write completion");
        const LikesProgram::Net::CompletionStats stats = loop.GetCompletionStats(); // 关闭后资源收敛
        const bool epollBackend = std::strcmp(
            loop.CompletionBackendName(),
            "epoll-level-completion") == 0; // graceful 语义共用，批量能力分支断言
        Require(stats.pendingDatagramSends == 0
                && stats.datagramSendCompletions >= 64,
            "UDP graceful Shutdown should converge every pending send");
        Require(epollBackend
                ? stats.maximumDatagramSendBatch == 1
                : stats.maximumDatagramSendBatch > 1,
            "UDP graceful Shutdown should retain backend-specific send evidence");
#endif
    }

    // UDP provided-buffer lease 必须支持暂停恢复、跨线程归还和晚于 Poller 释放。
    void TestUdpMultishotLeaseLifecycle() {
#if defined(LIKESPROGRAM_NET_TEST_HAS_IO_URING)
        LikesProgram::Net::Buffer retained; // 三条 lease 依次移入同一外部 Buffer
        LikesProgram::Net::CompletionStats finalStats; // Poller 销毁前的资源快照
        {
            auto loop = std::make_unique<LikesProgram::Net::EventLoop>(); // 允许显式先销毁 Poller
            if (IsEpollBackend(loop->CompletionBackendName())) {
                const LikesProgram::Net::CompletionStats stats = loop->GetCompletionStats();
                Require(stats.datagramProvidedBufferCount == 0
                        && stats.datagramActiveBufferLeases == 0
                        && stats.datagramPendingBufferReturns == 0,
                    "Epoll UDP should retain no provided-buffer lease lifecycle");
                return;
            }
            const std::size_t bufferCount =
                loop->GetCompletionStats().datagramProvidedBufferCount; // 完整归还目标水位
            Require(bufferCount >= 64,
                "UDP lease lifecycle requires the registered datagram buffer pool");
            const LikesProgram::Net::Address any("127.0.0.1", 0); // 独立 UDP 接收端口
            const int receiverFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Connection 接管
            Require(receiverFd >= 0, "UDP lease lifecycle receiver socket should open");
            Require(::bind(receiverFd, any.SockAddr(), any.Length()) == 0,
                "UDP lease lifecycle receiver should bind a loopback address");
            Require(SetTestNonBlocking(receiverFd),
                "UDP lease lifecycle receiver should enter nonblocking mode");
            const LikesProgram::Net::Address receiverAddress =
                LikesProgram::Net::Address::GetLocalAddress(receiverFd); // 原始 sender 目标

            const int senderFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // 测试线程独占
            Require(senderFd >= 0, "UDP lease lifecycle sender socket should open");
            std::atomic<std::size_t> received{ 0 }; // callback 发布暂停与恢复进度
            auto connection = std::make_shared<DatagramLeaseLifecycleConnection>(
                receiverFd,
                loop.get(),
                retained,
                received);
            loop->AttachConnection(connection);
            loop->PostTask([connection]() { connection->Start(); });
            std::thread worker([&loop]() { loop->Start(); });

            const char first[] = "first"; // 第一条 payload 在暂停期间跨回调持有
            Require(::sendto(
                    senderFd,
                    first,
                    sizeof(first) - 1,
                    0,
                    receiverAddress.SockAddr(),
                    receiverAddress.Length()) == static_cast<ssize_t>(sizeof(first) - 1),
                "UDP lease lifecycle should send the first datagram");
            const auto firstDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (received.load(std::memory_order_acquire) < 1
                && std::chrono::steady_clock::now() < firstDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(received.load(std::memory_order_acquire) == 1
                    && retained.AsStringView() == "first",
                "UDP lease lifecycle should retain the first callback payload");

            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 等待 multishot cancel terminal CQE
            const char second[] = "second"; // 暂停期间应留在 socket 接收队列
            Require(::sendto(
                    senderFd,
                    second,
                    sizeof(second) - 1,
                    0,
                    receiverAddress.SockAddr(),
                    receiverAddress.Length()) == static_cast<ssize_t>(sizeof(second) - 1),
                "UDP lease lifecycle should queue the paused datagram");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Require(received.load(std::memory_order_acquire) == 1,
                "Paused UDP multishot should suppress additional business callbacks");

            std::thread firstRelease([&retained]() {
                retained.RetrieveAll(); // 非 issuer 线程只排队 token，不触碰 buffer ring
            });
            firstRelease.join();
            connection->ResumeReading();
            const auto secondDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (received.load(std::memory_order_acquire) < 2
                && std::chrono::steady_clock::now() < secondDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (received.load(std::memory_order_acquire) < 2) {
                loop->Shutdown();
                worker.join();
                (void)::close(senderFd);
                Require(false,
                    "Resumed UDP multishot should deliver the queued datagram lease");
            }
            if (retained.AsStringView() != "second") {
                loop->Shutdown();
                worker.join();
                (void)::close(senderFd);
                Require(false,
                    "Second UDP lifecycle callback should retain the expected payload");
            }

            std::thread secondRelease([&retained]() {
                retained.RetrieveAll(); // 再次跨线程归还第二个 token，等待 issuer drain
            });
            secondRelease.join();
            loop->PostTask([]() { }); // 唤醒 issuer 并在下一轮 Flush 回填第二个 token
            LikesProgram::Net::CompletionStats convergedStats; // 第三条到达前的完整 pool 快照
            const auto convergenceDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                convergedStats = loop->GetCompletionStats();
                if (convergedStats.datagramActiveBufferLeases == 0
                    && convergedStats.datagramPendingBufferReturns == 0
                    && convergedStats.datagramCurrentAvailableBuffers == bufferCount) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < convergenceDeadline);
            if (convergedStats.datagramActiveBufferLeases != 0
                || convergedStats.datagramPendingBufferReturns != 0
                || convergedStats.datagramCurrentAvailableBuffers != bufferCount) {
                connection->ForceClose();
                loop->Shutdown();
                worker.join();
                (void)::close(senderFd);
                Require(false,
                    "Cross-thread UDP releases should restore every pool token before Poller destruction");
            }

            const char third[] = "third"; // 第三条 lease 将活过 Poller 析构
            const ssize_t thirdSent = ::sendto(
                senderFd,
                third,
                sizeof(third) - 1,
                0,
                receiverAddress.SockAddr(),
                receiverAddress.Length());
            if (thirdSent != static_cast<ssize_t>(sizeof(third) - 1)) {
                loop->Shutdown();
                worker.join();
                (void)::close(senderFd);
                Require(false, "UDP lease lifecycle should queue the third datagram");
            }
            connection->ResumeReading();
            const auto thirdDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (received.load(std::memory_order_acquire) < 3
                && std::chrono::steady_clock::now() < thirdDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (received.load(std::memory_order_acquire) < 3) loop->Shutdown();
            worker.join();
            (void)::close(senderFd);

            Require(received.load(std::memory_order_acquire) == 3
                    && retained.AsStringView() == "third",
                "Third UDP lifecycle callback should retain the Poller-lifetime payload");
            finalStats = loop->GetCompletionStats();
            Require(finalStats.datagramMultishotReceiveCompletions >= 3
                    && finalStats.pendingDatagramSends == 0,
                "UDP lease lifecycle stats should converge before Poller destruction");
            connection.reset();
            loop.reset(); // 第三条 lease 仍持有 pool 引用，不能访问已注销 ring
        }

        Require(retained.AsStringView() == "third",
            "UDP provided-buffer payload should outlive Poller destruction");
        std::thread finalRelease([&retained]() {
            retained.RetrieveAll(); // 已关闭 pool 只释放最后引用，不再排队 token
        });
        finalRelease.join();
        Require(retained.ReadableBytes() == 0,
            "UDP lease should release cleanly after Poller destruction");
#endif
    }

    // 真实 io_uring UDP completion 必须保留 peer、空包、截断和关闭边界。
    void TestUdpCompletionPreservesDatagramMetadata() {
#ifndef _WIN32
        DatagramObservation observation; // server issuer 发布每个数据报的元数据
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Udp,
            [&observation](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<DatagramEchoConnection>(fd, loop, observation);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses(); // 显式 SendTo 的固定 server peer
        Require(!addresses.empty(), "UDP completion server should expose a bound address");
        const LikesProgram::Net::Address serverAddress = addresses.front();

        int clients[2] = {
            ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP),
            ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        };
        Require(clients[0] >= 0 && clients[1] >= 0, "UDP completion clients should open");
        Require(SetTestNonBlocking(clients[0]) && SetTestNonBlocking(clients[1]),
            "UDP completion clients should enter nonblocking mode");

        const auto send = [&serverAddress](int fd, const char* data, std::size_t size) {
            return ::sendto(
                fd,
                data,
                size,
                0,
                serverAddress.SockAddr(),
                serverAddress.Length());
        };
        const auto receive = [](int fd, std::string& payload) {
            std::array<char, 32> storage{}; // 足够容纳本测试容量内回显
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                const ssize_t bytes = ::recv(fd, storage.data(), storage.size(), 0);
                if (bytes >= 0) {
                    payload.assign(storage.data(), static_cast<std::size_t>(bytes));
                    return true;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };

        Require(send(clients[0], "one", 3) == 3, "First UDP peer should send one datagram");
        Require(send(clients[1], "two", 3) == 3, "Second UDP peer should send one datagram");
        std::string firstPayload; // 第一个 client 只能收到自己的回显
        std::string secondPayload; // 第二个 client 只能收到自己的回显
        Require(receive(clients[0], firstPayload) && firstPayload == "one",
            "First UDP peer should receive its own explicit reply");
        Require(receive(clients[1], secondPayload) && secondPayload == "two",
            "Second UDP peer should receive its own explicit reply");

        Require(send(clients[0], "", 0) == 0, "UDP completion should submit an empty datagram");
        std::string emptyPayload; // recv 返回 0 仍表示收到一个 UDP 数据报
        Require(receive(clients[0], emptyPayload) && emptyPayload.empty(),
            "Zero-length UDP datagram should receive one empty reply");

        Require(send(clients[1], "truncate", 8) == 8,
            "UDP completion should submit an oversized test datagram");
        std::string truncatedPayload; // 4-byte 配置只交付容量内前缀
        Require(receive(clients[1], truncatedPayload) && truncatedPayload == "trun",
            "Truncated UDP datagram should preserve the capacity prefix");
        Require(observation.m_zeroLength.load(std::memory_order_acquire) == 1,
            "Zero-length UDP datagram should invoke OnDatagram exactly once");
        Require(observation.m_originalBytes.load(std::memory_order_acquire) == 8
                && observation.m_truncated.load(std::memory_order_acquire),
            "Truncated UDP callback should expose original wire length and flag");

        LikesProgram::Net::CompletionStats stats; // 等待最后一个 sendmsg CQE 更新可观测计数
        const auto statsDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            LikesProgram::Net::EventLoop* loop = observation.m_loop.load(std::memory_order_acquire);
            if (loop != nullptr) stats = loop->GetCompletionStats();
            if (stats.datagramReceiveCompletions >= 4
                && stats.datagramSendCompletions >= 4
                && stats.pendingDatagramSends == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < statsDeadline);
        Require(stats.datagramReceiveCompletions >= 4 && stats.receivedDatagrams >= 4,
            "UDP stats should count every recvmsg completion and delivered datagram");
        Require(stats.datagramSendCompletions >= 4 && stats.sentDatagrams >= 4,
            "UDP stats should count every completed sendmsg datagram");
        Require(stats.zeroLengthDatagrams >= 2 && stats.truncatedDatagrams >= 1,
            "UDP stats should count zero-length receive/send and truncation");
        Require(stats.pendingDatagramSends == 0,
            "UDP stats should converge pending sends after every reply CQE");
        Require(observation.m_received.load(std::memory_order_acquire) == 4,
            "UDP peer, zero-length, and truncation cases should each invoke one callback");

        const std::size_t completedBeforeShutdown = observation.m_received.load(
            std::memory_order_acquire); // 关闭后 late datagram 不得进入业务回调
        server.Shutdown();
        (void)send(clients[0], "late", 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Require(observation.m_received.load(std::memory_order_acquire) == completedBeforeShutdown,
            "UDP Shutdown should suppress late datagram callbacks");
        (void)::close(clients[0]);
        (void)::close(clients[1]);
#endif
    }

    void TestTcpClientCanRestartAfterConnectionClose() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses();
        Require(!addresses.empty(), "Client restart server should expose bound address");

        std::atomic<bool> received{ false }; // 第二条物理连接的回显完成标记
        std::string payload;
        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", addresses.front().Port()),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(fd, loop, received, payload);
            });

        client.Start();
        auto first = client.GetConnection();
        Require(first != nullptr, "Client restart should create the first connection");
        first->ForceClose();
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (client.GetStatus() != LikesProgram::Net::Client::Status::Stopped
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(client.GetStatus() == LikesProgram::Net::Client::Status::Stopped,
            "Closed client connection should publish Stopped before restart");

        client.Start(); // 必须先 join/清理旧 loopThread，再建立新 completion connect
        auto second = client.GetConnection();
        Require(second != nullptr && second != first,
            "Client restart should create a new connection without assigning over a joinable thread");
        second->Send("restart", 7);
        const auto receiveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < receiveDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        client.Shutdown();
        server.Shutdown();
        Require(received.load(std::memory_order_acquire) && payload == "restart",
            "Restarted client should complete TCP echo on the new connection");
    }

    void TestWorkerAcceptRunsOnOwnerLoop() {
        std::atomic<bool> factoryCalled{ false }; // 等待 worker listener 接受连接
        std::atomic<bool> factoryOnOwnerLoop{ false }; // factory 必须与连接 owner loop 同线程
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            [&factoryCalled, &factoryOnOwnerLoop](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                factoryOnOwnerLoop.store(
                    loop != nullptr && loop->IsInLoopThread(),
                    std::memory_order_release);
                factoryCalled.store(true, std::memory_order_release);
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.SetWorkerThreads(2);
        server.Start();

        const auto addresses = server.GetListenAddresses(); // port 0 必须先解析为共享实际端口
        Require(!addresses.empty(), "Worker accept server should expose a bound address");
        const LikesProgram::Net::Address remote("127.0.0.1", addresses.front().Port());
        const int clientFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // 真实 TCP 连接触发 accept
        Require(clientFd >= 0, "Worker accept client socket should open");
        Require(::connect(clientFd, remote.SockAddr(), remote.Length()) == 0,
            "Worker accept client should connect");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!factoryCalled.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        (void)::close(clientFd);
        server.Shutdown();
        Require(factoryCalled.load(std::memory_order_acquire),
            "Worker listener should invoke the connection factory");
        Require(factoryOnOwnerLoop.load(std::memory_order_acquire),
            "Worker listener should construct a connection on its owner loop thread");
    }

    void TestServerCanShutdownFromWorkerConnectionCallback() {
        LikesProgram::Net::Server* serverAddress = nullptr; // Start 前发布给 worker connection 回调
        std::atomic<bool> callbackRan{ false }; // 观察 OnConnected 内 Shutdown 是否返回
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [&serverAddress, &callbackRan](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<PoolShutdownOnConnectedConnection>(
                    fd,
                    loop,
                    [&serverAddress, &callbackRan]() {
                        serverAddress->Shutdown();
                        callbackRan.store(true, std::memory_order_release);
                    });
            });
        serverAddress = &server;
        server.SetWorkerThreads(1);
        server.Start();

        const auto addresses = server.GetListenAddresses();
        Require(!addresses.empty(), "Worker callback shutdown server should expose bound address");
        const int clientFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // 触发 worker multishot accept
        Require(clientFd >= 0, "Worker callback shutdown client socket should open");
        Require(::connect(clientFd, addresses.front().SockAddr(), addresses.front().Length()) == 0,
            "Worker callback shutdown client should connect");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!callbackRan.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)::close(clientFd);

        Require(callbackRan.load(std::memory_order_acquire),
            "Server Shutdown should return inside worker connection callback");
        Require(server.GetStatus() == LikesProgram::Net::Server::Status::Stopped,
            "Worker callback shutdown should converge Server status");
    }

    void TestTcpConnectionPoolReusesIdleConnection() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "ConnectionPool server should expose bound address");

        auto state = std::make_shared<PoolCaptureState>();
        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port());
        options.maxConnections = 1;
        options.maxIdleConnections = 1;
        options.workerThreads = 1;
        options.acquireTimeout = std::chrono::milliseconds(500);
        options.factory = LikesProgram::Net::ConnectionFactory(
            [state](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<PoolCaptureConnection>(fd, loop, state);
            });

        LikesProgram::Net::ConnectionPool pool(options);

        LikesProgram::Net::SocketType firstSocket = LikesProgram::Net::kInvalidSocket;
        {
            auto first = pool.Acquire();
            Require(static_cast<bool>(first), "ConnectionPool should acquire first TCP lease");
            firstSocket = first.Get()->GetSocket();
            first.Get()->Send("one", 3);
            Require(WaitPoolPayload(state, 0, "one"), "ConnectionPool first echo mismatch");

            const auto activeStats = pool.Stats();
            Require(activeStats.activeConnections == 1, "ConnectionPool active count mismatch");
            Require(activeStats.totalConnections == 1, "ConnectionPool total count mismatch");
            first.Release();
        }

        const auto idleStats = pool.Stats();
        Require(idleStats.activeConnections == 0, "ConnectionPool should release active lease");
        Require(idleStats.idleConnections == 1, "ConnectionPool should keep one idle connection");

        auto second = pool.Acquire();
        Require(static_cast<bool>(second), "ConnectionPool should acquire second TCP lease");
        Require(second.Get()->GetSocket() == firstSocket, "ConnectionPool should reuse idle TCP connection");
        second.Get()->Send("two", 3);
        Require(WaitPoolPayload(state, 1, "two"), "ConnectionPool second echo mismatch");
        second.Discard();

        const auto discardedStats = pool.Stats();
        Require(discardedStats.activeConnections == 0, "ConnectionPool discard should release active lease");
        Require(discardedStats.totalConnections == 0, "ConnectionPool discard should remove known connection");

        pool.Shutdown();
        server.Shutdown();
    }

    void TestConnectionPoolWaitTimeoutAndShutdown() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses(); // 为池提供真实 completion connect 目标
        Require(!addresses.empty(), "ConnectionPool wait server should expose bound address");

        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = addresses.front();
        options.maxConnections = 1;
        options.maxIdleConnections = 1;
        options.acquireTimeout = std::chrono::milliseconds(500);
        LikesProgram::Net::ConnectionPool pool(options);

        auto held = pool.Acquire(); // 占满唯一连接名额
        Require(static_cast<bool>(held), "ConnectionPool should acquire the held lease");
        const auto timeoutStart = std::chrono::steady_clock::now();
        auto timedOut = pool.TryAcquire(std::chrono::milliseconds(30));
        const auto timeoutElapsed = std::chrono::steady_clock::now() - timeoutStart;
        Require(!timedOut, "ConnectionPool TryAcquire should return empty at its deadline");
        Require(timeoutElapsed >= std::chrono::milliseconds(20),
            "ConnectionPool TryAcquire should wait for the configured deadline");

        std::atomic<bool> waiterFinished{ false }; // Shutdown 必须唤醒长等待者
        std::atomic<bool> waiterAcquired{ false }; // 关闭池后不能再交付连接
        std::thread waiter([&pool, &waiterFinished, &waiterAcquired]() {
            auto lease = pool.TryAcquire(std::chrono::seconds(5));
            waiterAcquired.store(static_cast<bool>(lease), std::memory_order_release);
            waiterFinished.store(true, std::memory_order_release);
        });
        const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (pool.Stats().waitingAcquires == 0
            && std::chrono::steady_clock::now() < waitDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        pool.Shutdown();
        waiter.join();
        held.Release(); // 租约可长于 Pool Shutdown，并安全归还共享状态
        server.Shutdown();

        Require(waiterFinished.load(std::memory_order_acquire),
            "ConnectionPool Shutdown should wake a waiting acquire");
        Require(!waiterAcquired.load(std::memory_order_acquire),
            "ConnectionPool Shutdown should not hand a connection to a waiter");
        const auto stats = pool.Stats();
        Require(stats.activeConnections == 0 && stats.totalConnections == 0,
            "ConnectionPool Shutdown should converge counters after lease release");
    }

    void TestConnectionPoolDoesNotReuseClosedConnection() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses(); // 关闭后重新建立真实 TCP completion
        Require(!addresses.empty(), "ConnectionPool closed server should expose bound address");

        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = addresses.front();
        options.maxConnections = 1;
        options.maxIdleConnections = 1;
        LikesProgram::Net::ConnectionPool pool(options);

        auto closedLease = pool.Acquire();
        Require(static_cast<bool>(closedLease), "ConnectionPool should acquire a connection to close");
        auto closedConnection = closedLease.Get(); // 保留对象以观察池触发的最终关闭
        closedConnection->Shutdown();
        Require(closedConnection->GetState() == LikesProgram::Net::Connection::State::Closing,
            "Connection Shutdown should publish Closing before returning");
        closedLease.Release(); // Closing 连接必须同步移出池，并由池触发 ForceClose
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (closedConnection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(closedConnection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "ConnectionPool should close a released Closing connection");

        const auto closedStats = pool.Stats();
        Require(closedStats.idleConnections == 0 && closedStats.totalConnections == 0,
            "ConnectionPool must remove Closed connections instead of idling them");
        auto replacement = pool.Acquire();
        Require(static_cast<bool>(replacement) && replacement.Get()->IsConnected(),
            "ConnectionPool should create a connected replacement after close");
        replacement.Discard();
        pool.Shutdown();
        server.Shutdown();
    }

    void TestConnectionPoolKeepsTlsSessionPerConnection() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop); // 回显测试握手记录以推进 Engine
            });
        server.Start();
        const auto addresses = server.GetListenAddresses();
        Require(!addresses.empty(), "ConnectionPool TLS server should expose bound address");

        std::atomic<int> engineCreateCount{ 0 }; // 每条物理连接必须独占一个 Engine 会话
        LikesProgram::Net::TlsEngineFactory tlsFactory([&engineCreateCount]() {
            engineCreateCount.fetch_add(1, std::memory_order_acq_rel);
            return std::make_unique<TestTlsEngine>();
        });
        auto sessionStates = std::make_shared<std::vector<std::shared_ptr<TlsCompletionState>>>();
        auto sessionMutex = std::make_shared<std::mutex>(); // 保护 issuer factory 与测试线程共享列表

        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = addresses.front();
        options.maxConnections = 1;
        options.maxIdleConnections = 1;
        options.factory = LikesProgram::Net::ConnectionFactory(
            [tlsFactory, sessionStates, sessionMutex](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                auto state = std::make_shared<TlsCompletionState>(); // 状态生命周期覆盖池内连接
                {
                    std::lock_guard<std::mutex> lock(*sessionMutex);
                    sessionStates->push_back(state);
                }
                return std::make_shared<TlsCompletionConnection>(fd, loop, *state, tlsFactory);
            });
        LikesProgram::Net::ConnectionPool pool(options);

        auto first = pool.Acquire();
        Require(static_cast<bool>(first), "ConnectionPool should acquire first TLS session");
        const auto firstHandshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (engineCreateCount.load(std::memory_order_acquire) < 1
            && std::chrono::steady_clock::now() < firstHandshakeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto firstConnection = first.Get(); // 同一物理连接复用时 Engine 指针不得重建
        first.Release();

        auto reused = pool.Acquire();
        Require(reused.Get() == firstConnection,
            "ConnectionPool should preserve the TLS session with its physical connection");
        Require(engineCreateCount.load(std::memory_order_acquire) == 1,
            "Reusing a TLS connection must not create a second Engine session");
        reused.Discard();

        auto replacement = pool.Acquire();
        const auto replacementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (engineCreateCount.load(std::memory_order_acquire) < 2
            && std::chrono::steady_clock::now() < replacementDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(replacement.Get() != firstConnection,
            "Discarded TLS connection should be replaced by a new connection object");
        Require(engineCreateCount.load(std::memory_order_acquire) == 2,
            "A replacement physical connection should receive a new Engine session");
        replacement.Discard();

        pool.Shutdown();
        server.Shutdown();
    }

    void TestConnectionPoolCanShutdownFromWorkerCallback() {
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            [](LikesProgram::Net::SocketType fd, LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<EchoConnection>(fd, loop);
            });
        server.Start();
        const auto addresses = server.GetListenAddresses();
        Require(!addresses.empty(), "ConnectionPool callback shutdown server should expose bound address");

        LikesProgram::Net::ConnectionPool* poolAddress = nullptr; // 构造后、Acquire 前发布给 factory 回调
        std::atomic<bool> callbackRan{ false }; // 证明关闭发生在 worker OnConnected 栈内
        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = addresses.front();
        options.maxConnections = 1;
        options.workerThreads = 1;
        options.factory = LikesProgram::Net::ConnectionFactory(
            [&poolAddress, &callbackRan](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<PoolShutdownOnConnectedConnection>(
                    fd,
                    loop,
                    [&poolAddress, &callbackRan]() {
                        callbackRan.store(true, std::memory_order_release);
                        poolAddress->Shutdown();
                    });
            });

        LikesProgram::Net::ConnectionPool pool(options);
        poolAddress = &pool;
        auto lease = pool.TryAcquire(std::chrono::seconds(2));
        server.Shutdown();

        Require(callbackRan.load(std::memory_order_acquire),
            "ConnectionPool worker OnConnected callback should run");
        Require(!lease,
            "ConnectionPool shutdown during connect callback must cancel lease delivery");
        const auto stats = pool.Stats();
        Require(stats.activeConnections == 0 && stats.totalConnections == 0,
            "Worker callback shutdown should converge pool counters without UAF");
    }

    void TestConnectionPoolRejectsUdpTransport() {
        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = LikesProgram::Net::Address("127.0.0.1", 9);
        options.transportKind = LikesProgram::Net::TransportKind::Udp;

        bool rejected = false; // 记录 TCP-only 约束是否被构造期拒绝
        try {
            LikesProgram::Net::ConnectionPool pool(options);
            (void)pool;
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }

        Require(rejected, "ConnectionPool should reject UDP transport kind");
    }

    void TestTcpConnectCompletionFailureReleasesState() {
        const int reservedFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // 先取得一个当前可用回环端口
        Require(reservedFd >= 0, "Connect failure endpoint socket should open");
        const LikesProgram::Net::Address requested("127.0.0.1", 0);
        Require(::bind(reservedFd, requested.SockAddr(), requested.Length()) == 0,
            "Connect failure endpoint should bind");
        const LikesProgram::Net::Address closedEndpoint =
            LikesProgram::Net::Address::GetLocalAddress(reservedFd);
        (void)::close(reservedFd); // 没有 listener 的端口应通过 connect CQE 返回拒绝

        LikesProgram::Net::Client client(
            closedEndpoint,
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory{});
        client.Start();
        Require(client.GetStatus() == LikesProgram::Net::Client::Status::Stopped,
            "Failed Client connect completion should converge to Stopped");
        Require(client.GetConnection() == nullptr,
            "Failed Client connect completion must not publish a connection");

        LikesProgram::Net::ConnectionPoolOptions options;
        options.remoteAddress = closedEndpoint;
        options.maxConnections = 1;
        options.acquireTimeout = std::chrono::milliseconds(500);
        LikesProgram::Net::ConnectionPool pool(options);
        bool poolFailed = false; // connect CQE 错误按现有 API 作为异常返回
        try {
            auto lease = pool.TryAcquire(std::chrono::milliseconds(500));
            poolFailed = !lease;
        }
        catch (const std::runtime_error&) {
            poolFailed = true;
        }

        const auto stats = pool.Stats();
        pool.Shutdown();
        Require(poolFailed,
            "ConnectionPool should report a refused connect completion");
        Require(stats.activeConnections == 0 && stats.totalConnections == 0,
            "Failed pool connect must release active and pending connection slots");
    }

    void TestServerContainsAcceptedConnectionStartFailure() {
        std::atomic<int> factoryCalls{ 0 }; // 首条连接失败、第二条连接回显
        std::mutex failedConnectionMutex; // 保护 factory worker 与测试线程共享 weak_ptr
        std::weak_ptr<LikesProgram::Net::Connection> failedConnection; // 首条失败连接状态
        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [&factoryCalls, &failedConnectionMutex, &failedConnection](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    const int call = factoryCalls.fetch_add(1, std::memory_order_acq_rel); // 当前 accept 次序
                    if (call == 0) {
                        auto connection = std::make_shared<ThrowingAcceptedConnection>(fd, loop);
                        {
                            std::lock_guard<std::mutex> lock(failedConnectionMutex);
                            failedConnection = connection;
                        }
                        return std::static_pointer_cast<LikesProgram::Net::Connection>(connection);
                    }
                    return std::static_pointer_cast<LikesProgram::Net::Connection>(
                        std::make_shared<EchoConnection>(fd, loop));
                }));
        server.Start();
        const auto addresses = server.GetListenAddresses(); // 获取实际回环端口
        Require(!addresses.empty(), "Accepted failure server should expose bound address");

        const int failedFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // 触发首个 throwing connection
        Require(failedFd >= 0, "Accepted failure client socket should open");
        Require(::connect(failedFd, addresses.front().SockAddr(), addresses.front().Length()) == 0,
            "Accepted failure client should connect");
        (void)::close(failedFd);

        const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool failureClosed = false; // Server 必须在 fd 可复用前完成统一关闭
        while (std::chrono::steady_clock::now() < failureDeadline) {
            std::shared_ptr<LikesProgram::Net::Connection> snapshot;
            {
                std::lock_guard<std::mutex> lock(failedConnectionMutex);
                snapshot = failedConnection.lock();
            }
            if (!snapshot || snapshot->GetState() == LikesProgram::Net::Connection::State::Closed) {
                failureClosed = factoryCalls.load(std::memory_order_acquire) >= 1;
                if (failureClosed) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(failureClosed,
            "Throwing accepted connection should close through the Connection owner path");

        std::atomic<bool> received{ false }; // 第二条连接证明 multishot accept/worker 仍可用
        std::string payload;
        LikesProgram::Net::Client client(
            addresses.front(),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(fd, loop, received, payload);
            });
        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Server should accept a replacement after start failure");
        connection->Send("survives", 8);
        const auto echoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < echoDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        client.Shutdown();
        server.Shutdown();
        Require(received.load(std::memory_order_acquire) && payload == "survives",
            "Server should continue accepting after a user start callback throws");
    }

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    void TestImmediateSecureUpgradeKeepsSocketFallback() {
        auto sharedContext = std::make_shared<StartTlsLikeTcpTransport::SharedSecureContext>();
        std::atomic<int> serverHandshakes{ 0 };

        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [sharedContext, &serverHandshakes](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    auto transport = std::make_unique<StartTlsLikeTcpTransport>(fd, sharedContext);
                    return std::make_shared<ImmediateUpgradeConnection>(
                        fd,
                        loop,
                        std::move(transport),
                        serverHandshakes);
                },
                [sharedContext]() {
                    sharedContext->loadCount.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }));

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Immediate secure server should expose bound address");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload);
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Immediate secure client should create connection");
        connection->Send("secure", 6);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), "Immediate secure echo should complete");
        Require(payload == "secure", "Immediate secure echo payload mismatch");
        Require(serverHandshakes.load(std::memory_order_acquire) > 0,
            "OnSecureLayerReady should trigger handshake before message handling");
        Require(sharedContext->loadCount.load(std::memory_order_acquire) == 1,
            "Shared secure context should not be loaded once per connection");
    }

#endif

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    void TestSharedSecureResourcesInitializeOnceBeforeConnections() {
        auto state = std::make_shared<SharedSecureFactoryState>();

        LikesProgram::Net::Server server(
            LikesProgram::Net::Address("127.0.0.1", 0),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [state](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
                        "Shared secure resources should initialize before connection creation");
                    state->connectionCreateCount.fetch_add(1, std::memory_order_acq_rel);
                    return std::make_shared<EchoConnection>(fd, loop);
                },
                [state]() {
                    state->sharedInitCount.fetch_add(1, std::memory_order_acq_rel);
                    return true;
                }));

        server.Start();
        const auto listenAddresses = server.GetListenAddresses();
        Require(!listenAddresses.empty(), "Shared secure server should expose bound address");

        std::atomic<bool> received{ false };
        std::string payload;

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", listenAddresses.front().Port()),
            LikesProgram::Net::TransportKind::Tcp,
            [&received, &payload](
                LikesProgram::Net::SocketType fd,
                LikesProgram::Net::EventLoop* loop) {
                return std::make_shared<CaptureConnection>(
                    fd,
                    loop,
                    received,
                    payload);
            });

        client.Start();
        auto connection = client.GetConnection();
        Require(connection != nullptr, "Shared secure client should create connection");
        connection->Send("shared", 6);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        client.Shutdown();
        server.Shutdown();

        Require(received.load(std::memory_order_acquire), "Shared secure echo should complete");
        Require(payload == "shared", "Shared secure echo payload mismatch");
        Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
            "Shared secure resources should initialize exactly once");
        Require(state->connectionCreateCount.load(std::memory_order_acquire) == 1,
            "Server should create one accepted connection");
    }

    void TestSharedSecureResourceFailureStopsClient() {
        auto state = std::make_shared<SharedSecureFactoryState>();

        LikesProgram::Net::Client client(
            LikesProgram::Net::Address("127.0.0.1", 9),
            LikesProgram::Net::TransportKind::Tcp,
            LikesProgram::Net::ConnectionFactory(
                [state](
                    LikesProgram::Net::SocketType fd,
                    LikesProgram::Net::EventLoop* loop) {
                    (void)fd;
                    (void)loop;
                    state->connectionCreateCount.fetch_add(1, std::memory_order_acq_rel);
                    return std::shared_ptr<LikesProgram::Net::Connection>{};
                },
                [state]() {
                    state->sharedInitCount.fetch_add(1, std::memory_order_acq_rel);
                    return false;
                }));

        client.Start();

        Require(client.GetStatus() == LikesProgram::Net::Client::Status::Stopped,
            "Client should stop when shared secure resources fail");
        Require(client.GetConnection() == nullptr,
            "Client should not create connection after shared secure failure");
        Require(state->sharedInitCount.load(std::memory_order_acquire) == 1,
            "Shared secure failure initializer should run once");
        Require(state->connectionCreateCount.load(std::memory_order_acquire) == 0,
            "Connection factory should not run after shared secure failure");
    }
#endif

    void TestEventLoopPostTaskWakesImmediately() {
        LikesProgram::Net::EventLoop loop;
        std::atomic<bool> loopEntered{ false };
        std::atomic<bool> done{ false };

        loop.SetPollTimeout(500);
        std::thread worker([&loop, &loopEntered]() {
            loopEntered.store(true, std::memory_order_release);
            loop.Start();
        });

        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!loopEntered.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Require(loopEntered.load(std::memory_order_acquire), "EventLoop thread should start");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto begin = std::chrono::steady_clock::now();
        loop.PostTask([&done]() {
            done.store(true, std::memory_order_release);
        });

        const auto taskDeadline = begin + std::chrono::milliseconds(100);
        while (!done.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < taskDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        loop.Shutdown();
        worker.join();

        Require(done.load(std::memory_order_acquire), "PostTask should wake loop immediately");
    }

#if defined(LIKESPROGRAM_NET_ENABLE_LEGACY_TRANSPORT_TESTS)
    void TestClosingReusedFdKeepsReplacementConnection() {
        LikesProgram::Net::EventLoop loop;
        const LikesProgram::Net::SocketType reusedFd = static_cast<LikesProgram::Net::SocketType>(987654); // 模拟复用键
        std::shared_ptr<LikesProgram::Net::Connection> replacement; // Close 内创建的新连接快照
        std::atomic<bool> closeFinished{ false }; // 确认 owner loop 已完成关闭任务

        auto oldConnection = std::make_shared<LikesProgram::Net::Connection>(
            reusedFd,
            &loop,
            std::make_unique<ReuseFdOnCloseTcpTransport>(
                reusedFd,
                [&loop, &replacement](LikesProgram::Net::SocketType fd) {
                    // 旧连接尚在关闭栈上时，模拟 accept 获得相同 fd 并挂入同一 worker。
                    replacement = std::make_shared<LikesProgram::Net::Connection>(
                        fd,
                        &loop,
                        std::make_unique<BlockingTcpTransport>(fd));
                    loop.AttachConnection(replacement);
                }));
        loop.AttachConnection(oldConnection);

        std::thread worker([&loop]() { loop.Start(); });
        oldConnection->ForceClose();
        loop.PostTask([&closeFinished]() {
            closeFinished.store(true, std::memory_order_release);
        });
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!closeFinished.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const std::shared_ptr<LikesProgram::Net::Connection> retained = loop.DetachConnection(reusedFd); // 查询复用后的条目
        loop.Shutdown();
        worker.join();

        Require(closeFinished.load(std::memory_order_acquire), "Owner loop should finish the close task");
        Require(retained == replacement,
            "Closing an old connection must not detach a replacement that reused the same fd");
    }

    void TestMovedBufferSendTriggersWriteWatermark() {
        LikesProgram::Net::EventLoop loop;
        std::atomic<bool> loopEntered{ false };
        std::atomic<int> highWatermarkCount{ 0 };
        std::atomic<int> overflowCount{ 0 };
        std::atomic<std::size_t> pendingBytes{ 0 };

        auto connection = std::make_shared<BackpressureCaptureConnection>(
            LikesProgram::Net::kInvalidSocket,
            &loop,
            std::make_unique<BlockingTcpTransport>(LikesProgram::Net::kInvalidSocket),
            highWatermarkCount,
            overflowCount,
            pendingBytes);
        connection->SetWriteWatermark(4, 2);

        loop.SetPollTimeout(500);
        std::thread worker([&loop, &loopEntered]() {
            loopEntered.store(true, std::memory_order_release);
            loop.Start();
        });

        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!loopEntered.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        LikesProgram::Net::Buffer payload;
        payload.Append("abcdef", 6);
        connection->Send(std::move(payload));

        const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (highWatermarkCount.load(std::memory_order_acquire) == 0
            && std::chrono::steady_clock::now() < callbackDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        connection->ForceClose();
        loop.Shutdown();
        worker.join();

        Require(highWatermarkCount.load(std::memory_order_acquire) == 1,
            "Moved Buffer send should trigger high watermark once");
        Require(overflowCount.load(std::memory_order_acquire) == 0,
            "High watermark should not imply hard overflow");
        Require(pendingBytes.load(std::memory_order_acquire) >= 6,
            "High watermark should report pending bytes");
    }

    void TestWriteQueueHardLimitClosesSlowConnection() {
        LikesProgram::Net::EventLoop loop;
        std::atomic<bool> loopEntered{ false };
        std::atomic<int> highWatermarkCount{ 0 };
        std::atomic<int> overflowCount{ 0 };
        std::atomic<std::size_t> pendingBytes{ 0 };

        auto connection = std::make_shared<BackpressureCaptureConnection>(
            LikesProgram::Net::kInvalidSocket,
            &loop,
            std::make_unique<BlockingTcpTransport>(LikesProgram::Net::kInvalidSocket),
            highWatermarkCount,
            overflowCount,
            pendingBytes);
        connection->SetMaxPendingWriteBytes(5);

        loop.SetPollTimeout(500);
        std::thread worker([&loop, &loopEntered]() {
            loopEntered.store(true, std::memory_order_release);
            loop.Start();
        });

        const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!loopEntered.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        LikesProgram::Net::Buffer payload;
        payload.Append("abcdef", 6);
        connection->Send(std::move(payload));

        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection->GetState() != LikesProgram::Net::Connection::State::Closed
            && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        loop.Shutdown();
        worker.join();

        Require(overflowCount.load(std::memory_order_acquire) == 1,
            "Hard write queue limit should report overflow once");
        Require(connection->GetState() == LikesProgram::Net::Connection::State::Closed,
            "Hard write queue limit should close slow connection");
        Require(pendingBytes.load(std::memory_order_acquire) >= 6,
            "Overflow should report pending bytes");
    }
#endif
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::strcmp(argv[1], "--backend-contract") == 0) {
            const char* requestedBackend = argc >= 3
                ? argv[2]
                : nullptr; // 默认 focused 调用锁定 auto/io_uring 诊断名
            return RunBackendContract(requestedBackend);
        }
#if defined(__linux__)
        if (argc >= 2 && std::strcmp(argv[1], "--connect-accept-contract") == 0) {
            // Task 6 只复用不依赖 TCP/UDP Connection 数据链的既有真实回归。
            TestCompletionConnectUsesPollerOperation();
            TestCompletionConnectCancelSuppressesCallback();
            TestCompletionMultishotAcceptUsesSingleSubmission();
            TestTcpConnectCompletionFailureReleasesState();
            std::cout << "PackageTests connect/accept contract passed\n";
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--tcp-tls-contract") == 0) {
            // Task 7 复用真实 socketpair、partial send、TLS timer 与 close_notify 回归。
            TestCompletionBackendExecutesPlainTcpIo();
            TestTlsEngineRunsOnCompletionDataPath();
            TestTlsEngineSupportsManualUpgrade();
            TestTlsHandshakeTimeoutUsesCompletionOperation();
            TestTlsHandshakeCompletionCancelsTimeout();
            TestRealCompletionPartialSendDrainsBeforeShutdown();
            std::cout << "PackageTests TCP/TLS contract passed\n";
            return 0;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--udp-dtls-contract") == 0) {
            // Task 8 共用 peer、边界、timer、MTU、关闭与队列收敛语义，只分支能力统计。
            TestUdpCompletionPreservesDatagramMetadata();
            TestUdpMultishotReceiveBatch();
            TestUdpLinkedSendBatch();
            TestUdpLinkedSendForceCloseWhileActive();
            TestUdpLinkedSendGracefulShutdown();
            TestDtlsTimersMtuAndCloseSemantics();
            TestDtlsPeerCiphertextRetiresIndependently();
            TestRealDtlsConnectedClientDataPath();
            TestRealDtlsUnconnectedServerDataPath();
            std::cout << "PackageTests UDP/DTLS contract passed\n";
            return 0;
        }
#endif

        TestPackageIdentity();
        TestTlsEngineContractAndFactory();
        TestQueuedSendBeforeShutdownDrains();
        TestQueuedTlsSendPrecedesCloseNotify();
        TestOutboundTlsEngineExceptionsCloseConnection();
        TestTlsTimeoutObserverExceptionStillClosesConnection();
        TestStartingCallbackFastPathRequiresSameThread();
        TestStartingCallbackFlagClearsOnException();
        TestDefaultPollerExposesBackendName();
#if defined(__linux__)
        TestCompletionConnectUsesPollerOperation();
        TestCompletionConnectCancelSuppressesCallback();
#endif
        TestEventLoopExposesCompletionBackendName();
        TestEventLoopActivationFailureRunsStartupTasks();
        TestEventLoopContainsThrowingPendingTask();
        TestEventLoopShutdownBeforeStartCannotReviveLoop();
        TestEventLoopDrainsShutdownCompletions();
        TestCompletionTimeoutFiresAndCancels();
        TestCompletionTimeoutPreservesBudgetAcrossSignalInterrupts();
        TestCompletionBackendExecutesPlainTcpIo();
        TestTlsEngineRunsOnCompletionDataPath();
        TestTlsEngineSupportsManualUpgrade();
        TestTlsHandshakeTimeoutUsesCompletionOperation();
        TestTlsHandshakeCompletionCancelsTimeout();
        TestCompletionMultishotReceiveUsesSingleSubmission();
        TestCompletionReceiveBundleConsumesMultipleBuffers();
        TestCompletionMultishotAcceptUsesSingleSubmission();
        TestCompletionBudgetAdaptsAfterLowUtilization();
        TestCompletionLeaseOutlivesPollerClose();
        TestProvidedBufferGenerationReconfigurationPreservesLeases();
        TestBuffer();
        TestBufferLeaseAdoptionIsNoexcept();
        TestBufferLeaseMoveAssignmentReleasesExactlyOnce();
        TestBufferLeaseAvoidsReceiveCopy();
        TestOwningBufferCompletionUsesConnectionInputPath();
        TestCompletionCallbackExceptionClosesAndReturnsLease();
        TestCompletionWatermarkExceptionClosesConnection();
        TestCompletionWriteSubmissionObserverExceptionClosesConnection();
        TestBufferLeaseMaterializesAcrossLifetimeBoundary();
        TestProvidedBufferPoolReturnsOnDrainThread();
        TestProvidedBufferPoolConcurrentReturns();
        TestProvidedBufferPoolOutlivesPollerClose();
        TestProvidedBufferPolicySeparatesCqCongestionFromBufferPressure();
        TestOperationCancellationPolicyRetriesWithoutDuplicates();
        TestConnectionIdentityPolicyRejectsStaleCloseCallback();
        TestBufferChainOwnsAndConsumesLeaseSegments();
        TestConnectionMovesBufferIntoPollerWrite();
        TestConnectedDatagramSendUsesConnectedSocket();
        TestDtlsClientStartsOnConnectedUdp();
        TestDtlsRejectsInvalidTransportConfigurations();
        TestDtlsServerRoutesInterleavedPeers();
        TestDtlsServerBoundsAndIsolatesPeerFailures();
        TestDtlsTimersMtuAndCloseSemantics();
        TestDtlsPeerCiphertextRetiresIndependently();
        TestRealDtlsConnectedClientDataPath();
        TestRealDtlsUnconnectedServerDataPath();
        TestDatagramPollerBridgePreservesMetadata();
        TestCompletionBackpressureControlsReadAndClose();
        TestCompletionShutdownRejectsLateWrites();
        TestRealCompletionPartialSendDrainsBeforeShutdown();
        TestTcpSmallReadKeepsBufferBounded();
        TestAddress();
        TestUdpSendToExplicitPeer();
        TestUdpBatchSendReceive();
#if defined(__linux__)
        TestTcpWriteToClosedPeerDoesNotRaiseSigpipe();
#endif
        TestEventLoopPostTaskWakesImmediately();
        TestRoundTrip(LikesProgram::Net::TransportKind::Tcp, "Client should receive TCP echo");
        TestRoundTrip(
            LikesProgram::Net::TransportKind::Tcp,
            "Client should receive TCP echo through worker reactor",
            2);
        TestTcpClientCanRestartAfterConnectionClose();
        TestWorkerAcceptRunsOnOwnerLoop();
        TestServerCanShutdownFromWorkerConnectionCallback();
        TestRoundTrip(LikesProgram::Net::TransportKind::Udp, "Client should receive UDP echo");
        TestUdpMultishotReceiveBatch();
        TestUdpEnobufsWaitsForReturnedToken();
        TestUdpLinkedSendBatch();
        TestUdpLinkedSendForceCloseWhileActive();
        TestUdpLinkedSendGracefulShutdown();
        TestUdpMultishotLeaseLifecycle();
        TestUdpCompletionPreservesDatagramMetadata();
        TestTcpConnectionPoolReusesIdleConnection();
        TestConnectionPoolWaitTimeoutAndShutdown();
        TestConnectionPoolDoesNotReuseClosedConnection();
        TestConnectionPoolKeepsTlsSessionPerConnection();
        TestConnectionPoolCanShutdownFromWorkerCallback();
        TestConnectionPoolRejectsUdpTransport();
        TestTcpConnectCompletionFailureReleasesState();
#if defined(__linux__)
        TestServerContainsAcceptedConnectionStartFailure();
#endif
        std::cout << "PackageTests passed\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "PackageTests failed: " << ex.what() << '\n';
        return 1;
    }
}
