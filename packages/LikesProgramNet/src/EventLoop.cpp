#include <LikesProgram/Net/EventLoop.hpp>
#include "net/platform/EventLoopWakeup.hpp"
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace LikesProgram {
    namespace Net {
        struct EventLoop::EventLoopImpl {
            std::unique_ptr<Poller> m_poller;                         // 当前事件循环独占的轮询器
            std::atomic<bool> m_running{ false };                     // 主循环运行标志
            std::atomic<bool> m_shutdownRequested{ false };           // Start 前后的单调关闭闸门
            std::atomic<bool> m_threadIdSet{ false };                 // loop 线程 id 是否有效
            std::thread::id m_loopThreadId{};                         // Start 所在线程 id
            std::unordered_map<SocketType, std::shared_ptr<Connection>> m_connections; // 活跃连接持有
            std::mutex m_connectionMutex;                             // 保护连接持有表
            std::vector<Task> m_pendingTasks;                         // 跨线程投递任务队列
            std::mutex m_taskMutex;                                   // 保护任务队列
            std::atomic<bool> m_hasPendingTasks{ false };              // 无任务时跳过互斥量，并防止唤醒丢失
            std::unique_ptr<Internal::EventLoopWakeup> m_wakeup;       // 跨线程唤醒句柄，打断阻塞中的 Poller。
            std::unique_ptr<Channel> m_wakeupChannel;                  // 绑定唤醒可读端的内部 Channel。
            int m_pollTimeoutMs = 10;                                 // completion 等待超时毫秒
        };

        EventLoop::EventLoop()
            : EventLoop(CreateDefaultPoller(this)) {
        }

        EventLoop::EventLoop(std::unique_ptr<Poller> poller)
            : m_impl(new EventLoopImpl{}) {
            m_impl->m_poller = std::move(poller);
            if (!m_impl->m_poller) m_impl->m_poller = CreateDefaultPoller(this);
            m_impl->m_poller->SetEventLoop(this);
            m_impl->m_wakeup = std::make_unique<Internal::EventLoopWakeup>();
            if (m_impl->m_wakeup->IsValid()) {
                m_impl->m_wakeupChannel = std::make_unique<Channel>(
                    this,
                    m_impl->m_wakeup->ReadSocket(),
                    IOEvent::Read);
                m_impl->m_wakeupChannel->SetReadCallback([this]() {
                    if (m_impl && m_impl->m_wakeup) m_impl->m_wakeup->Drain();
                });
                (void)m_impl->m_poller->AddChannel(m_impl->m_wakeupChannel.get());
            }
        }

        EventLoop::~EventLoop() {
            Shutdown();
            if (m_impl != nullptr && m_impl->m_wakeupChannel && m_impl->m_poller) {
                // 先取消唤醒 poll，再释放 Channel 与平台 Poller。
                (void)m_impl->m_poller->RemoveChannel(m_impl->m_wakeupChannel.get());
                m_impl->m_wakeupChannel.reset();
            }
            delete m_impl;
            m_impl = nullptr;
        }

        void EventLoop::Start() {
            if (!m_impl) return;

            m_impl->m_loopThreadId = std::this_thread::get_id();
            m_impl->m_threadIdSet.store(true, std::memory_order_release);
            if (m_impl->m_shutdownRequested.load(std::memory_order_acquire)) {
                // Shutdown 先于 worker 调度时只执行取消/启动通知，不得重新进入主循环。
                ProcessPendingTasks();
                m_impl->m_threadIdSet.store(false, std::memory_order_release);
                return;
            }
            if (!m_impl->m_poller->Activate()) {
                // 执行预先排队的启动屏障/失败任务，避免 Client 与 EventLoopGroup 永久等待。
                ProcessPendingTasks();
                m_impl->m_threadIdSet.store(false, std::memory_order_release);
                return;
            }
            if (m_impl->m_wakeupChannel
                && m_impl->m_wakeupChannel->GetIndex() != Channel::Index::Added) {
                // readiness Poller 只有 Activate 后才能注册唤醒 fd，补齐构造期无法登记的通道。
                if (!m_impl->m_poller->AddChannel(m_impl->m_wakeupChannel.get())) {
                    ProcessPendingTasks();
                    m_impl->m_threadIdSet.store(false, std::memory_order_release);
                    return;
                }
            }
            m_impl->m_running.store(true, std::memory_order_release);
            if (m_impl->m_shutdownRequested.load(std::memory_order_acquire)) {
                // 覆盖 Activate 与 running 发布之间的 Shutdown 竞态。
                m_impl->m_running.store(false, std::memory_order_release);
            }

            std::vector<Channel*> activeChannels; // 本轮活跃 Channel 集合
            while (m_impl->m_running.load(std::memory_order_acquire)) {
                m_impl->m_poller->Poll(m_impl->m_pollTimeoutMs, activeChannels);
                ProcessEvents(activeChannels);
                ProcessPendingTasks();
                m_impl->m_poller->Flush();
            }

            ProcessPendingTasks();
            m_impl->m_poller->Flush();
            while (m_impl->m_poller->HasPendingShutdownCompletions()) {
                // 关闭任务提交 cancel 后继续轮询，直到原 operation 的 terminal CQE 回收状态。
                m_impl->m_poller->Poll(m_impl->m_pollTimeoutMs, activeChannels);
                ProcessEvents(activeChannels);
                ProcessPendingTasks();
                m_impl->m_poller->Flush();
            }
            m_impl->m_threadIdSet.store(false, std::memory_order_release);
        }

        void EventLoop::Shutdown() {
            if (!m_impl) return;

            m_impl->m_shutdownRequested.store(true, std::memory_order_release);
            m_impl->m_running.store(false, std::memory_order_release);
            if (m_impl->m_wakeup) (void)m_impl->m_wakeup->Wakeup();
        }

        bool EventLoop::IsRunning() const noexcept {
            return m_impl && m_impl->m_running.load(std::memory_order_acquire);
        }

        bool EventLoop::IsInLoopThread() const noexcept {
            return m_impl
                && m_impl->m_threadIdSet.load(std::memory_order_acquire)
                && std::this_thread::get_id() == m_impl->m_loopThreadId;
        }

        const char* EventLoop::CompletionBackendName() const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->BackendName()
                : "none";
        }

        std::uint64_t EventLoop::CompletionOperationCount() const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->CompletedOperationCount()
                : 0;
        }

        std::size_t EventLoop::CompletionProvidedBufferCount() const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->ProvidedBufferCount()
                : 0;
        }

        bool EventLoop::CompletionReceiveBundleEnabled() const noexcept {
            return m_impl != nullptr
                && m_impl->m_poller
                && m_impl->m_poller->ReceiveBundleEnabled();
        }

        CompletionStats EventLoop::GetCompletionStats() const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->GetCompletionStats()
                : CompletionStats{};
        }

        std::uint64_t EventLoop::CompletionReadSubmissionCount() const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->ReadSubmissionCount()
                : 0;
        }

        bool EventLoop::StartPollerIo(Connection* connection) {
            if (m_impl == nullptr || connection == nullptr || !m_impl->m_poller) return false;

            std::shared_ptr<Connection> snapshot; // completion CQE 回收前持有同一连接对象
            {
                std::lock_guard<std::mutex> lock(m_impl->m_connectionMutex);
                const auto found = m_impl->m_connections.find(connection->GetSocket());
                if (found == m_impl->m_connections.end() || found->second.get() != connection) return false;
                snapshot = found->second;
            }
            return m_impl->m_poller->StartConnection(snapshot);
        }

        bool EventLoop::StartPollerAccept(SocketType listenFd, Poller::AcceptCallback callback) {
            return m_impl != nullptr
                && m_impl->m_poller
                && m_impl->m_poller->StartAccept(listenFd, std::move(callback));
        }

        void EventLoop::StopPollerAccept(SocketType listenFd) {
            if (m_impl && m_impl->m_poller) m_impl->m_poller->StopAccept(listenFd);
        }

        Poller::TimeoutId EventLoop::SchedulePollerTimeout(
            std::chrono::milliseconds delay,
            Task task) {
            if (m_impl == nullptr || !m_impl->m_poller || !task) {
                return Poller::InvalidTimeoutId;
            }
            return m_impl->m_poller->ScheduleTimeout(delay, std::move(task));
        }

        void EventLoop::CancelPollerTimeout(Poller::TimeoutId timeoutId) noexcept {
            if (m_impl == nullptr || !m_impl->m_poller
                || timeoutId == Poller::InvalidTimeoutId) return;
            m_impl->m_poller->CancelTimeout(timeoutId);
        }

        bool EventLoop::QueuePollerWrite(Connection* connection, Buffer&& buffer) {
            return m_impl != nullptr
                && m_impl->m_poller
                && m_impl->m_poller->QueueWrite(connection, std::move(buffer));
        }

        bool EventLoop::QueuePollerWrite(Connection* connection, BufferChain&& chain) {
            return m_impl != nullptr
                && m_impl->m_poller
                && m_impl->m_poller->QueueWrite(connection, std::move(chain));
        }

        bool EventLoop::QueuePollerDatagramWrite(
            Connection* connection,
            const Address& peer,
            Buffer&& buffer) {
            return m_impl != nullptr
                && m_impl->m_poller
                && m_impl->m_poller->QueueDatagramWrite(connection, peer, std::move(buffer));
        }

        void EventLoop::SetPollerReadEnabled(Connection* connection, bool enabled) {
            if (m_impl && m_impl->m_poller) m_impl->m_poller->SetReadEnabled(connection, enabled);
        }

        std::size_t EventLoop::PollerPendingWriteBytes(const Connection* connection) const noexcept {
            return m_impl != nullptr && m_impl->m_poller
                ? m_impl->m_poller->PendingWriteBytes(connection)
                : 0;
        }

        void EventLoop::StopPollerIo(Connection* connection) {
            if (m_impl && m_impl->m_poller) m_impl->m_poller->StopConnection(connection);
        }

        bool EventLoop::RegisterChannel(Channel* channel) {
            if (m_impl == nullptr || channel == nullptr) return false;
            return m_impl->m_poller->AddChannel(channel);
        }

        bool EventLoop::UnregisterChannel(Channel* channel) {
            if (m_impl == nullptr || channel == nullptr) return false;
            return m_impl->m_poller->RemoveChannel(channel);
        }

        bool EventLoop::UpdateChannel(Channel* channel) {
            if (m_impl == nullptr || channel == nullptr) return false;
            return m_impl->m_poller->UpdateChannel(channel);
        }

        void EventLoop::PostTask(Task task) {
            if (m_impl == nullptr || !task) return;

            bool shouldWakeup = false; // 只在队列从空变为非空时通知一次 Poller
            {
                std::lock_guard<std::mutex> lock(m_impl->m_taskMutex); // 先入队再唤醒，确保 loop 被叫醒后能看到任务。
                shouldWakeup = m_impl->m_pendingTasks.empty();
                m_impl->m_pendingTasks.push_back(std::move(task));
                m_impl->m_hasPendingTasks.store(true, std::memory_order_release);
            }

            if (shouldWakeup && !IsInLoopThread() && m_impl->m_wakeup) {
                // loop 线程会在本轮事件后处理任务，无需向自己的 eventfd 再写一次。
                (void)m_impl->m_wakeup->Wakeup();
            }
        }

        void EventLoop::AttachConnection(const std::shared_ptr<Connection>& connection) {
            if (m_impl == nullptr || !connection) return;

            std::lock_guard<std::mutex> lock(m_impl->m_connectionMutex); // 保护连接生命周期表
            m_impl->m_connections[connection->GetSocket()] = connection;
        }

        std::shared_ptr<Connection> EventLoop::DetachConnection(SocketType fd) {
            if (m_impl == nullptr) return {};

            std::lock_guard<std::mutex> lock(m_impl->m_connectionMutex); // 保护连接生命周期表
            const auto it = m_impl->m_connections.find(fd);
            if (it == m_impl->m_connections.end()) return {};

            std::shared_ptr<Connection> connection = it->second; // 返回快照延长关闭流程生命周期
            m_impl->m_connections.erase(it);
            return connection;
        }

        void EventLoop::SetPollTimeout(int timeoutMs) noexcept {
            if (m_impl) m_impl->m_pollTimeoutMs = timeoutMs < 0 ? 0 : timeoutMs;
        }

        void EventLoop::ProcessEvents(const std::vector<Channel*>& activeChannels) {
            for (Channel* channel : activeChannels) {
                if (channel != nullptr) channel->HandleEvent();
            }
        }

        void EventLoop::ProcessPendingTasks() {
            if (m_impl == nullptr
                || !m_impl->m_hasPendingTasks.load(std::memory_order_acquire)) {
                return;
            }

            std::vector<Task> tasks; // 本轮待执行任务快照
            {
                std::lock_guard<std::mutex> lock(m_impl->m_taskMutex);
                tasks.swap(m_impl->m_pendingTasks);
                m_impl->m_hasPendingTasks.store(false, std::memory_order_release);
            }

            for (auto& task : tasks) {
                if (!task) continue;
                try {
                    // 单个用户任务失败不能截断后续取消、关闭与资源归还任务。
                    task();
                }
                catch (...) {
                    // EventLoop 是后台 issuer 边界，任务异常不得击穿 completion 主循环。
                }
            }
        }

        Poller& EventLoop::PollerRef() {
            return *m_impl->m_poller;
        }
    }
}
