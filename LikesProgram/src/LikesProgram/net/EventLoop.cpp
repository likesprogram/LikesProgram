#include "../../../include/LikesProgram/net/EventLoop.hpp"
#include "../../../include/LikesProgram/net/Connection.hpp"
#include "../../../include/LikesProgram/net/IOEvent.hpp"
#include <cassert>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace LikesProgram {
	namespace Net {

#ifndef _WIN32
        static inline void SetNonBlocking(SocketType fd) {
            int flags = ::fcntl((int)fd, F_GETFL, 0);
            if (flags >= 0) ::fcntl((int)fd, F_SETFL, flags | O_NONBLOCK);
        }
#endif

        EventLoop::EventLoop(std::unique_ptr<Poller> poller)
            : m_poller(std::move(poller)) {
            assert(m_poller && "EventLoop requires a valid Poller");
            InitWakeup();
        }

        EventLoop::~EventLoop() {
            // EventLoop 不拥有线程，析构前必须确保 Run 已退出
            // 否则这里对 poller/channel/fd 的操作会与 Run 并发，产生竞态
            assert(!m_running.load(std::memory_order_acquire) && "Destroy EventLoop only after Stop() and Run() has exited.");

#ifndef _WIN32
            if (m_hasWakeup) {
                if (m_wakeupChannel) {
                    // 析构时不应再跨线程操作 poller
                    (void)m_poller->RemoveChannel(m_wakeupChannel.get());
                    m_wakeupChannel.reset();
                }
                if (m_wakeupReadFd != (SocketType)0)  ::close((int)m_wakeupReadFd);
                if (m_wakeupWriteFd != (SocketType)0) ::close((int)m_wakeupWriteFd);
                m_hasWakeup = false;
            }
#endif
        }

        bool EventLoop::IsInLoopThread() const noexcept {
            if (!m_threadIdSet.load(std::memory_order_acquire)) return false;
            return std::this_thread::get_id() == m_loopThreadId;
        }

        void EventLoop::SetLoopThreadIdOnce() {
            bool expected = false;
            if (m_threadIdSet.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                m_loopThreadId = std::this_thread::get_id();
            }
        }

        void EventLoop::Run() {
            SetLoopThreadIdOnce();
            m_running.store(true, std::memory_order_release);

            // 有 wakeup：可以无限阻塞，靠 Wakeup() 打断
            const int pollTimeout = m_hasWakeup ? -1 : m_pollTimeoutMs;
            std::vector<Channel*> active;
            while (m_running.load(std::memory_order_acquire)) {
                active.clear();

                // poll
                m_poller->Poll(pollTimeout, active);

                // dispatch
                if (!active.empty()) ProcessEvents(active);

                // tasks
                ProcessPendingTasks();
            }

            // 退出前清空任务（Stop 后 PostTask 可能仍进来）
            ProcessPendingTasks();
        }

        void EventLoop::Stop() {
            // 如果从其他线程 Stop，需要 wakeup 打断 poll
            if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
            Wakeup();
        }

        void EventLoop::PostTask(Task task) {
            if (!task) return;

            {
                std::lock_guard<std::mutex> lk(m_tasksMutex);
                m_pendingTasks.push_back(std::move(task));
            }

            // 只有非 loop 线程才需要唤醒（loop 线程会在本轮结束时处理 tasks）
            if (!IsInLoopThread()) {
                Wakeup();
            }
        }

        void EventLoop::ProcessPendingTasks() {
            std::vector<Task> tasks;
            {
                std::lock_guard<std::mutex> lk(m_tasksMutex);
                if (m_pendingTasks.empty()) return;
                tasks.swap(m_pendingTasks);
            }

            for (auto& t : tasks) {
                if (t) t();
            }
        }

        bool EventLoop::RegisterChannel(Channel* channel) {
            if (!channel) return false;

            if (!IsInLoopThread()) {
                // 约定：channel 生命周期由 Connection/loop 保证（AttachConnection 先于注册，且关闭时串行）
                PostTask([this, channel]() { (void)RegisterChannel(channel); });
                return true; // queued
            }

            return m_poller->AddChannel(channel);
        }

        bool EventLoop::UpdateChannel(Channel* channel) {
            if (!channel) return false;

            if (!IsInLoopThread()) {
                PostTask([this, channel]() { (void)UpdateChannel(channel); });
                return true;
            }

            return m_poller->UpdateChannel(channel);
        }

        bool EventLoop::UnregisterChannel(Channel* channel) {
            if (!channel) return false;

            if (!IsInLoopThread()) {
                PostTask([this, channel]() { (void)UnregisterChannel(channel); });
                return true;
            }

            return m_poller->RemoveChannel(channel);
        }

        void EventLoop::AttachConnection(const std::shared_ptr<Connection>& c) {
            if (!c) return;

            if (!IsInLoopThread()) {
                PostTask([this, c]() { AttachConnection(c); });
                return;
            }

            std::lock_guard<std::mutex> lk(m_connMutex);
            m_connections[c->GetSocket()] = c;
        }

        void EventLoop::DetachConnection(SocketType fd) {
            if (!IsInLoopThread()) {
                PostTask([this, fd]() { DetachConnection(fd); });
                return;
            }

            std::lock_guard<std::mutex> lk(m_connMutex);
            m_connections.erase(fd);
        }

        void EventLoop::ProcessEvents(const std::vector<Channel*>& activeChannels) {
            for (Channel* ch : activeChannels) {
                if (!ch) continue;

#ifndef _WIN32
                if (m_hasWakeup && m_wakeupChannel && ch == m_wakeupChannel.get()) {
                    HandleWakeupRead();
                    continue;
                }
#endif

                const SocketType fd = ch->GetSocket();

                std::shared_ptr<Connection> conn;
                {
                    std::lock_guard<std::mutex> lk(m_connMutex);
                    auto it = m_connections.find(fd);
                    if (it != m_connections.end()) conn = it->second;
                }
                if (!conn) continue;

                const IOEvent ev = ch->Revents();

                // 优先级：Error/Close 优先
                if ((ev & IOEvent::Error) != IOEvent::None) {
                    conn->HandleError();
                    continue;
                }
                if ((ev & IOEvent::Close) != IOEvent::None) {
                    conn->HandleClose();
                    continue;
                }

                if ((ev & IOEvent::Timeout) != IOEvent::None) {
                    conn->HandleTimeout();
                    // 不 continue：允许本轮同时处理 Read/Write
                }

                if ((ev & IOEvent::Read) != IOEvent::None) {
                    conn->HandleRead();
                }
                if ((ev & IOEvent::Write) != IOEvent::None) {
                    conn->HandleWrite();
                }
            }
        }

        void EventLoop::InitWakeup() {
#ifdef _WIN32
            // WindowsSelectPoller 无法用 pipe fd；先靠 pollTimeoutMs 兜底
            m_hasWakeup = false;
#else
            int fds[2]{ -1, -1 };
            if (::pipe(fds) != 0) {
                m_hasWakeup = false;
                return;
            }

            m_wakeupReadFd = (SocketType)fds[0];
            m_wakeupWriteFd = (SocketType)fds[1];

            SetNonBlocking(m_wakeupReadFd);
            SetNonBlocking(m_wakeupWriteFd);

            m_wakeupChannel = std::make_unique<Channel>(this, m_wakeupReadFd, IOEvent::Read, nullptr);
            (void)m_poller->AddChannel(m_wakeupChannel.get());

            m_hasWakeup = true;
#endif
        }

        void EventLoop::Wakeup() {
#ifndef _WIN32
            if (m_hasWakeup) {
                uint8_t b = 1;
                for (;;) {
                    const auto n = ::write((int)m_wakeupWriteFd, &b, 1);
                    if (n == 1) return;
                    if (n < 0 && errno == EINTR) continue;
                    // EAGAIN：pipe 满了，说明已经足够唤醒；直接返回
                    return;
                }
            }
#endif
            // fallback：无法中断 poll 时只能靠 pollTimeoutMs
        }

        void EventLoop::HandleWakeupRead() {
#ifndef _WIN32
            uint8_t buf[256];
            for (;;) {
                const auto n = ::read((int)m_wakeupReadFd, buf, sizeof(buf));
                if (n > 0) continue;
                if (n < 0 && errno == EINTR) continue;
                // n==0 或 EAGAIN：读空
                break;
            }
#endif
        }
	}
}
