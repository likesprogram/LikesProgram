#include "../../../include/LikesProgram/server/EventLoop.hpp"
#include <vector>

namespace LikesProgram {
    namespace Server {
        EventLoop::EventLoop(Poller* poller) : m_poller(poller), m_running(false) {
        }

        EventLoop::~EventLoop() {
            Stop();
        }

        void EventLoop::SetMainThreadPool(std::shared_ptr<LikesProgram::ThreadPool> pool) {
            m_mainPool = pool; // 指向 MainEventLoop 的线程池
        }

        bool EventLoop::RegisterChannel(Channel* channel) {
            if (!channel) return false;
            {
                std::lock_guard<std::mutex> lock(m_channelMutex); // 加锁
                if (m_channels.find(channel) != m_channels.end()) return false;

                m_channels.insert(channel);
                return m_poller->AddChannel(channel);
            }
        }

        bool EventLoop::UnregisterChannel(Channel* channel) {
            if (!channel) return false;
            {
                std::lock_guard<std::mutex> lock(m_channelMutex);  // 加锁
                auto it = m_channels.find(channel);
                if (it == m_channels.end()) return false;

                m_channels.erase(it);
                return m_poller->RemoveChannel(channel);
            }
        }

        bool EventLoop::UpdateChannel(Channel* channel) {
            if (!channel) return false;
            {
                std::lock_guard<std::mutex> lock(m_channelMutex);  // 加锁
                return m_poller->UpdateChannel(channel);
            }
        }

        void EventLoop::Run() {
            m_running.store(true, std::memory_order_relaxed); // 设置运行标志
            while (m_running.load(std::memory_order_relaxed)) { // 循环直到停止
                // 从 Poller 获取活跃 Channel 列表，超时 10ms
                auto activeChannels = m_poller->Dispatch(10);
                // 派发事件
                ProcessEvents(activeChannels);               // 处理事件
                // 执行异步任务
                ProcessPendingTasks();                       // 执行 EventLoop 内部任务
            }
        }

        void EventLoop::PostTask(const std::function<void()>& task) {
            if (!task) return;
            std::lock_guard<std::mutex> lock(m_taskMutex);
            m_pendingTasks.push_back(task);
        }

        void EventLoop::PostHeavyTask(const std::function<void()>& task) {
            if (m_mainPool) {
                m_mainPool->PostNoArg(task);
            }
            else {
                PostTask(task); // 没有线程池，降级为内部顺序执行
            }
        }

        void EventLoop::ProcessEvents(const std::vector<Channel*>& activeChannels) {
            for (Channel* ch : activeChannels) {
                if (!ch) continue;
                auto conn = ch->GetConnection();
                if (!conn) continue;

                // 触发读事件
                if (ch->IsEventEnabled(IOEvent::Read)) {
                    conn->Readable();
                }

                // 触发写事件
                if (ch->IsEventEnabled(IOEvent::Write) && ch->IsWriteEnabled()) {
                    conn->Writable();
                }
            }
        }

        void EventLoop::ProcessPendingTasks() {
            std::vector<std::function<void()>> tasks;
            {
                std::lock_guard<std::mutex> lock(m_taskMutex); // 保护任务队列
                tasks.swap(m_pendingTasks);                     // 交换到本地变量，减少锁持有时间
            }
            for (auto& task : tasks) {                         // 执行任务
                task();
            }
        }

        void EventLoop::Stop() {
            m_running.store(false);
        }

    } // namespace Server
} // namespace LikesProgram
