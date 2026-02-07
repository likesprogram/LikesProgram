#include "../../../include/LikesProgram/net/MainEventLoop.hpp"
#include "../../../include/LikesProgram/net/Connection.hpp"
#include "../../../include/LikesProgram/net/Server.hpp"
#include <cassert>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace LikesProgram {
    namespace Net {
        static inline void SetNonBlocking(SocketType fd) {
#ifdef _WIN32
            u_long mode = 1;
            ::ioctlsocket(fd, FIONBIO, &mode);
#else
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
        }

        static inline void CloseSocket(SocketType fd) {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        }
        static size_t DefaultSubLoopCount() {
            auto hc = std::thread::hardware_concurrency();
            return hc ? static_cast<size_t>(hc) : 1;
        }

        MainEventLoop::MainEventLoop(PollerFactory subPollerFactory,
            ConnectionFactory subConnectionFactory,
            size_t subLoopCount)
            : EventLoop(subPollerFactory()),
            m_subPollerFactory(std::move(subPollerFactory)),
            m_subConnectionFactory(subConnectionFactory),
            m_subLoopCount(subLoopCount ? subLoopCount : DefaultSubLoopCount()) {

            assert(m_subPollerFactory && "subPollerFactory required");
            assert(m_subConnectionFactory && "subConnectionFactory required");
            assert(m_subLoopCount > 0);

            // 预创建 sub loops
            m_subLoops.reserve(m_subLoopCount);
            for (size_t i = 0; i < m_subLoopCount; ++i) {
                auto poller = m_subPollerFactory();
                assert(poller && "sub poller factory returned null");
                m_subLoops.push_back(std::make_shared<EventLoop>(std::move(poller)));
            }
        }

        MainEventLoop::~MainEventLoop() {
            StopSubLoops();
        }

        void MainEventLoop::StartSubLoops() {
            if (m_subStarted) return;
            m_subStarted = true;

            m_subThreads.reserve(m_subLoops.size());
            for (auto& loop : m_subLoops) {
                m_subThreads.emplace_back([loop]() {
                    loop->Run();
                });
            }
        }

        void MainEventLoop::StopSubLoops() {
            if (!m_subStarted) return;

            for (auto& loop : m_subLoops) {
                loop->Stop();
            }
            for (auto& t : m_subThreads) {
                if (t.joinable()) t.join();
            }
            m_subThreads.clear();
            m_subStarted = false;
        }

        std::shared_ptr<EventLoop> MainEventLoop::PickSubLoopRoundRobin() {
            size_t idx = m_rr.fetch_add(1, std::memory_order_relaxed);
            return m_subLoops[idx % m_subLoops.size()];
        }

        void MainEventLoop::ProcessEvents(const std::vector<Channel*>& activeChannels) {
            // 约定：main loop 注册的 channel 就是 listen socket 的 channel
            // activeChannels 里每个可读事件都表示有新连接可 accept
            for (Channel* ch : activeChannels) {
                if (!ch) continue;

                // main loop 只关心 Read（accept）
                if ((ch->Revents() & IOEvent::Read) == IOEvent::None) continue;

                // 得到 fd，然后投递给 sub loop 创建 Connection
                SocketType listenFd = ch->GetSocket();

                while (true) {
                    SocketType clientFd;

#ifdef _WIN32
                    clientFd = ::accept(listenFd, nullptr, nullptr);
                    if (clientFd == INVALID_SOCKET) {
                        int e = ::WSAGetLastError();
                        if (e == WSAEWOULDBLOCK) break; // 没有更多连接了
                        // 其他错误
                        break;
                    }
                    SetNonBlocking(clientFd);
#else
                    // Linux 优先 accept4（如编译环境不支持，可改用 accept + SetNonBlocking）
                    clientFd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (clientFd < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        // 其他错误
                        break;
                    }
#endif

                    // Round-robin 选择 sub loop
                    auto loop = PickSubLoopRoundRobin();
                    auto connFactory = m_subConnectionFactory; // 拷贝，避免捕获 this
                    // 把连接创建 + 注册 Channel 投递到 sub loop 线程执行
                    loop->PostTask([loop, clientFd, connFactory]() {
                        // 在 sub loop 线程里创建 Connection（确保 loop 归属正确）
                        auto conn = connFactory(clientFd, loop.get());
                        if (!conn) {
                            CloseSocket(clientFd);
                            return;
                        }

                        // 让 sub loop 持有 conn，避免 Channel 里的裸指针悬空
                        loop->AttachConnection(conn);
                        std::weak_ptr<EventLoop> wloop = loop;
                        conn->SetFrameworkCloseCallback([wloop](Connection& c) {
                            if (auto s = wloop.lock()) s->DetachConnection(c.GetSocket());
                        });

                        // Channel 由 Connection 持有，避免泄漏
                        auto ch = std::make_unique<Channel>(loop.get(), conn->GetSocket(), IOEvent::Read, conn.get());
                        conn->SetChannel(ch.get());
                        if (!loop->RegisterChannel(ch.get())) {
                            // 注册失败：回滚
                            loop->DetachConnection(conn->GetSocket()); // 你这个函数会投递到 loop 线程也行，但此处已在 loop 线程
                            conn->FailedRollback();
                            // 关闭 socket
                            CloseSocket(clientFd);
                            return;
                        }
                        conn->AdoptChannel(std::move(ch));

                        // 连接完成
                        conn->Start();
                    });
                }
            }
        }
    }
}
