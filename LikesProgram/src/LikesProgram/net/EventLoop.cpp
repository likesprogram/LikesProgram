#include "../../../include/LikesProgram/net/EventLoop.hpp"
#include "../../../include/LikesProgram/net/Connection.hpp"
#include "../../../include/LikesProgram/net/IOEvent.hpp"
#include <iostream>
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

        EventLoop::EventLoop(std::unique_ptr<Poller> poller) : m_poller(std::move(poller)) {
            assert(m_poller && "EventLoop requires a valid Poller");
            InitWakeup();
        }

        EventLoop::~EventLoop() {
            assert(!m_running.load(std::memory_order_acquire) &&
                "Destroy EventLoop only after Stop() and Run() has exited.");

            // 先从 poller 移除 channel，再关闭句柄
            if (m_hasWakeup) {
                if (m_wakeupChannel) {
                    (void)m_poller->RemoveChannel(m_wakeupChannel.get());
                    m_wakeupChannel.reset();
                }

#ifdef _WIN32
                if (m_wakeupSock != kInvalidSocket) {
                    ::closesocket(m_wakeupSock);
                    m_wakeupSock = kInvalidSocket;
                }
#else
                if (m_wakeupReadFd != kInvalidSocket) {
                    ::close((int)m_wakeupReadFd);
                    m_wakeupReadFd = kInvalidSocket;
                }
                if (m_wakeupWriteFd != kInvalidSocket) {
                    ::close((int)m_wakeupWriteFd);
                    m_wakeupWriteFd = kInvalidSocket;
                }
#endif
                m_hasWakeup = false;
            }
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

            // 兜底：清空连接持有，避免任何路径漏 Detach
            {
                std::lock_guard<std::mutex> lk(m_connMutex);
                m_connections.clear();
            }
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
            if (!IsInLoopThread() || m_processingTasks.load(std::memory_order_relaxed)) {
                Wakeup();
            }
        }

        void EventLoop::ProcessPendingTasks() {
            m_processingTasks.store(true, std::memory_order_release);

            std::vector<Task> tasks;
            {
                std::lock_guard<std::mutex> lk(m_tasksMutex);
                if (m_pendingTasks.empty()) {
                    m_processingTasks.store(false, std::memory_order_release);
                    return;
                }
                tasks.swap(m_pendingTasks);
            }

            // 预算，避免一轮执行太久（尤其任务爆量时）
            constexpr size_t kMaxTasksPerTick = 1024;
            size_t n = 0;

            for (auto& t : tasks) {
                if (t) t();
                if (++n >= kMaxTasksPerTick) {
                    // 剩余任务塞回队列头/尾（这里简单塞回尾）
                    {
                        std::lock_guard<std::mutex> lk(m_tasksMutex);
                        // 把没跑完的部分移回 pending
                        for (size_t i = n; i < tasks.size(); ++i) {
                            if (tasks[i]) m_pendingTasks.push_back(std::move(tasks[i]));
                        }
                    }
                    // 让 loop 尽快再跑一轮，同时不给 IO 饿死
                    m_processingTasks.store(false, std::memory_order_release);
                    Wakeup();
                    return;
                }
            }

            m_processingTasks.store(false, std::memory_order_release);
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

        void EventLoop::BroadcastLocalExcept(const void* data, size_t len, const std::vector<SocketType>& removeSockets) {
            // 确保运行在 loop 中
            if (!IsInLoopThread()) {
                auto self = weak_from_this();
                Buffer copy;
                copy.Append(data, len);
                PostTask([self, buf = std::move(copy), removeSocketsTemp = removeSockets]() {
                    if (auto loop = self.lock()) loop->BroadcastLocalExcept(buf.Peek(), buf.ReadableBytes(), removeSocketsTemp);
                });
                return;
            }

            // 处理移除项 并发送
            if (removeSockets.empty()) {
                // 无排除对象：广播给所有当前存在的连接
                for (auto& [fd, c] : m_connections) if (c) c->Send(data, len);
            }
            else if (removeSockets.size() == 1) {
                // 只排除一个 fd（最常见：排除发送者自身）
                auto except = removeSockets[0];
                for (auto& [fd, c] : m_connections) if (c && fd != except) c->Send(data, len);
            }
            else {
                // 排除多个 fd：
                // 使用临时集合加速 contains 判断
                std::unordered_set<SocketType> removed(removeSockets.begin(), removeSockets.end());
                for (auto& [fd, c] : m_connections) if (c && !removed.count(fd)) c->Send(data, len);
            }
        }

        void EventLoop::ProcessEvents(const std::vector<Channel*>& activeChannels) {
            for (Channel* ch : activeChannels) {
                if (!ch) continue;

                if (m_hasWakeup && m_wakeupChannel && ch == m_wakeupChannel.get()) {
                    HandleWakeupRead();
                    continue;
                }

                SocketType fd = ch->GetSocket();

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
            m_hasWakeup = false;

            SocketType s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kInvalidSocket) return;

            // 绑定到 127.0.0.1:0（系统分配端口）
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;

            if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::closesocket(s);
                return;
            }

            // 取回实际端口
            int len = sizeof(addr);
            if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
                ::closesocket(s);
                return;
            }

            // 非阻塞
            u_long nb = 1;
            ::ioctlsocket(s, FIONBIO, &nb);

            m_wakeupSock = s;
            m_wakeupAddr = addr;

            // 把 wakeupSock 当成普通可读 Channel 加进 poller
            m_wakeupChannel = std::make_unique<Channel>(this, m_wakeupSock, IOEvent::Read, nullptr);
            (void)m_poller->AddChannel(m_wakeupChannel.get());

            m_hasWakeup = true;
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
#ifdef _WIN32
            if (!m_hasWakeup || m_wakeupSock == kInvalidSocket) return;

            uint8_t b = 1;
            (void)::sendto(
                m_wakeupSock,
                reinterpret_cast<const char*>(&b),
                1,
                0,
                reinterpret_cast<const sockaddr*>(&m_wakeupAddr),
                sizeof(m_wakeupAddr)
            );
#else
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
#ifdef _WIN32
            if (m_wakeupSock == kInvalidSocket) return;

            uint8_t buf[256];
            for (;;) {
                sockaddr_in from{};
                int fromlen = sizeof(from);
                int n = ::recvfrom(
                    m_wakeupSock,
                    reinterpret_cast<char*>(buf),
                    static_cast<int>(sizeof(buf)),
                    0,
                    reinterpret_cast<sockaddr*>(&from),
                    &fromlen
                );
                if (n > 0) continue;

                const int err = WSAGetLastError();
                if (err == WSAEINTR) continue;
                if (err == WSAEWOULDBLOCK) break;
                break;
            }
#else
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
