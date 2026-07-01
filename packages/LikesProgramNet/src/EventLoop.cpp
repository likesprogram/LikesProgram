#include <LikesProgram/Net/EventLoop.hpp>
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
            std::atomic<bool> m_threadIdSet{ false };                 // loop 线程 id 是否有效
            std::thread::id m_loopThreadId{};                         // Start 所在线程 id
            std::unordered_map<SocketType, std::shared_ptr<Connection>> m_connections; // 活跃连接持有
            std::mutex m_connectionMutex;                             // 保护连接持有表
            std::vector<Task> m_pendingTasks;                         // 跨线程投递任务队列
            std::mutex m_taskMutex;                                   // 保护任务队列
            int m_pollTimeoutMs = 10;                                 // select 等待超时毫秒
        };

        EventLoop::EventLoop()
            : EventLoop(CreateDefaultPoller(this)) {
        }

        EventLoop::EventLoop(std::unique_ptr<Poller> poller)
            : m_impl(new EventLoopImpl{}) {
            m_impl->m_poller = std::move(poller);
            if (!m_impl->m_poller) m_impl->m_poller = CreateDefaultPoller(this);
            m_impl->m_poller->SetEventLoop(this);
        }

        EventLoop::~EventLoop() {
            Shutdown();
            delete m_impl;
            m_impl = nullptr;
        }

        void EventLoop::Start() {
            if (!m_impl) return;

            m_impl->m_loopThreadId = std::this_thread::get_id();
            m_impl->m_threadIdSet.store(true, std::memory_order_release);
            m_impl->m_running.store(true, std::memory_order_release);

            std::vector<Channel*> activeChannels; // 本轮活跃 Channel 集合
            while (m_impl->m_running.load(std::memory_order_acquire)) {
                m_impl->m_poller->Poll(m_impl->m_pollTimeoutMs, activeChannels);
                ProcessEvents(activeChannels);
                ProcessPendingTasks();
            }

            ProcessPendingTasks();
            m_impl->m_threadIdSet.store(false, std::memory_order_release);
        }

        void EventLoop::Shutdown() {
            if (m_impl) m_impl->m_running.store(false, std::memory_order_release);
        }

        bool EventLoop::IsInLoopThread() const noexcept {
            return m_impl
                && m_impl->m_threadIdSet.load(std::memory_order_acquire)
                && std::this_thread::get_id() == m_impl->m_loopThreadId;
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

            std::lock_guard<std::mutex> lock(m_impl->m_taskMutex); // 保护跨线程任务队列
            m_impl->m_pendingTasks.push_back(std::move(task));
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
            std::vector<Task> tasks; // 本轮待执行任务快照
            {
                if (m_impl == nullptr) return;

                std::lock_guard<std::mutex> lock(m_impl->m_taskMutex);
                tasks.swap(m_impl->m_pendingTasks);
            }

            for (auto& task : tasks) {
                if (task) task();
            }
        }

        Poller& EventLoop::PollerRef() {
            return *m_impl->m_poller;
        }
    }
}
