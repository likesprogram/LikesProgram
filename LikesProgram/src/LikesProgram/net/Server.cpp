#include "../../../include/LikesProgram/net/Server.hpp"
#include "../../../include/LikesProgram/net/IOEvent.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#include "../../../include/LikesProgram/net/pollers/WindowsSelectPoller.hpp"
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include "../../../include/LikesProgram/net/pollers/EpollPoller.hpp"
#endif
#include <stdexcept>
#include <string>
#include <iostream>

namespace LikesProgram {
    namespace Net {
#ifdef _WIN32
        struct WinsockGlobal {
            WinsockGlobal() {
                WSADATA wsaData;
                if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                    throw std::runtime_error("WSAStartup failed");
                }
            }
            ~WinsockGlobal() {
                WSACleanup();
            }
        };
        WinsockGlobal& EnsureWinsock() {
            static WinsockGlobal g;
            return g;
        }
#endif
        static inline void CloseSocket(SocketType fd) {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        }

        static PollerFactory DefaultPollerFactory() {
#if defined(_WIN32)
            return []() -> std::unique_ptr<Poller> { return std::make_unique<WindowsSelectPoller>(nullptr); };
#else
            return []() -> std::unique_ptr<Poller> { return std::make_unique<EpollPoller>(nullptr); };
#endif
        }

        Server::Server(const Address& listenAddr, ConnectionFactory connectionFactory, size_t subLoopCount)
        : Server(std::vector<Address>{listenAddr}, DefaultPollerFactory(), std::move(connectionFactory), subLoopCount) { }

        Server::Server(const std::vector<Address>& listenAddrs, ConnectionFactory connectionFactory, size_t subLoopCount)
        : Server(listenAddrs, DefaultPollerFactory(), std::move(connectionFactory), subLoopCount) { }

        Server::Server(const Address& listenAddr, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount)
        : Server(std::vector<Address>{listenAddr}, std::move(pollerFactory), std::move(connectionFactory), subLoopCount) { }

        Server::Server(const std::vector<Address>& listenAddrs, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount) {
#ifdef _WIN32
            (void)EnsureWinsock();
#endif
            m_listenAddrs.clear();

            // 解析并去重
            std::unordered_set<std::string> seen;
            for (const auto& addr : listenAddrs) {
                auto resolved = Address::Resolve(addr.Ip(), addr.Port());
                for (auto& a : resolved) {
                    std::string key = a.ToString();  // ip:port
                    if (seen.insert(key).second) m_listenAddrs.push_back(std::move(a));
                }
            }

            // 监听 socket
            if (!Listen()) {
                throw std::runtime_error("Failed to listen");
            }

            // 创建主事件循环
            m_mainLoop = std::make_shared<MainEventLoop>(
                std::move(pollerFactory),
                std::move(connectionFactory),
                subLoopCount
            );

            // 注入广播器
            m_mainLoop->SetBroadcast(std::make_shared<Broadcast>());

            // 将监听通道注册到主循环中
            for (auto fd : m_listenFds) {
                auto ch = std::make_unique<Channel>(
                    m_mainLoop.get(),
                    fd,
                    IOEvent::Read,
                    nullptr
                );

                m_mainLoop->RegisterChannel(ch.get());
                m_listenChannels.push_back(std::move(ch));
            }
        }

        Server::~Server() {
            Shutdown();
            if (m_mainLoop) {
                for (auto& ch : m_listenChannels) {
                    if (ch) m_mainLoop->UnregisterChannel(ch.get());
                }
            }
            m_listenChannels.clear(); // 释放 Channel


            for (auto fd : m_listenFds) if (fd != kInvalidSocket) CloseSocket(fd);
            m_listenFds.clear();
        }

        void Server::Start() {
            {
                std::lock_guard<std::mutex> lk(m_stateMutex);
                if (!m_mainLoop) return; // 主事件循环不存在，直接退出

                // 已经在运行/启动中/正在停止，直接返回
                if (StatusAnyOf(Status::Running, Status::Starting, Status::Stopping)) return;

                SetStatus(Status::Starting);
            }

            // 如果之前线程对象还存在，等待关闭
            if (m_mainThread.joinable()) {
                if (m_mainThread.get_id() != std::this_thread::get_id()) m_mainThread.join();
                else return;
            }

            // 等待结束，判断环境是否变化
            std::lock_guard<std::mutex> lk(m_stateMutex);
            if (!m_mainLoop) return;
            if (!StatusEquals(Status::Starting)) return;

            // 启动线程
            m_mainThread = std::thread([this]() {
                try {
                    // 启动子循环
                    m_mainLoop->StartSubLoops();

                    // 检查状态被修改
                    if (StatusAnyOf(Status::Stopping, Status::Stopped)) {
                        m_mainLoop->ShutdownSubLoops();
                        return;
                    }

                    // 设置标志位
                    SetStatus(Status::Running);

                    // 启动主事件循环
                    m_mainLoop->Start();
                } catch (...) { /* 吃掉错误 */ }

                // 通知主循环退出
                try {
                    m_mainLoop->Shutdown(); // 防止意外退出，这里重复调用一次 Shutdown
                    m_mainLoop->ShutdownSubLoops();
                } catch (...) { /* 吃掉错误 */ }

                SetStatus(Status::Stopped);
            });
        }

        void Server::WaitShutdown() const noexcept {
            std::unique_lock<std::mutex> lk(m_stateMutex);

            m_stateCv.wait(lk, [this]() { return StatusEquals(Status::Stopped); });
        }

        void Server::Shutdown() {
            {
                std::lock_guard<std::mutex> lk(m_stateMutex);
                if (!m_mainLoop) return; // 主事件循环不存在，直接退出

                // 已经在停止/启动中/正在停止，直接返回
                if (StatusAnyOf(Status::Stopped, Status::Stopping)) return;

                SetStatus(Status::Stopping);

                // 通知主循环退出
                try {
                    m_mainLoop->Shutdown();
                } catch (...) { /* 吃掉错误 */ }

                if (!m_mainThread.joinable()) return;
            }
            // 等待 线程结束
            if (m_mainThread.get_id() != std::this_thread::get_id()) m_mainThread.join();
        }

        Server::Status Server::GetStatus() const noexcept {
            return m_status.load(std::memory_order_acquire);
        }

        std::shared_ptr<Broadcast> Server::GetBroadcast() noexcept {
            return m_mainLoop->GetBroadcast();
        }

        bool Server::Listen() {
            for (auto fd: m_listenFds) if (fd != kInvalidSocket) CloseSocket(fd);
            m_listenFds.clear();

            addrinfo* result = nullptr;

            for (const auto& addr : m_listenAddrs) {
                SocketType fd = (SocketType)::socket(addr.FamilyValue(), SOCK_STREAM, IPPROTO_TCP);
                if (fd == kInvalidSocket) {
#ifdef _WIN32
                    std::cout << "socket() failed family=" << addr.FamilyValue()
                        << " wsa=" << WSAGetLastError() << "\n";
#else
                    std::cout << "socket() failed family=" << addr.FamilyValue()
                        << " errno=" << errno << " " << std::strerror(errno) << "\n";
#endif
                    continue;
                }

                // 复用地址（用 int，不要用 char）
                int reuse = 1;
#ifdef _WIN32
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

                // IPv6 dual-stack（失败不致命，记录一下即可）
#ifdef IPV6_V6ONLY
                if (addr.FamilyValue() == AF_INET6) {
#ifdef _WIN32
                    DWORD v6only = 1;
                    if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                        (const char*)&v6only, sizeof(v6only)) != 0) {
                        std::cout << "setsockopt(IPV6_V6ONLY=0) failed wsa=" << WSAGetLastError() << "\n";
                    }
#else
                    int v6only = 1;
                    if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
                        std::cout << "setsockopt(IPV6_V6ONLY=0) failed errno=" << errno
                            << " " << std::strerror(errno) << "\n";
                    }
#endif
                }
#endif

                // bind
                if (::bind(fd, addr.SockAddr(), addr.Length()) != 0) {
#ifdef _WIN32
                    const int e = WSAGetLastError();
                    std::cout << "bind() failed family=" << addr.FamilyValue() << " wsa=" << e << "\n";
#else
                    const int e = errno;
                    std::cout << "bind() failed family=" << addr.FamilyValue()
                        << " errno=" << e << " " << std::strerror(e) << "\n";
#endif
                    CloseSocket(fd);
                    continue;
                }

                if (::listen(fd, 128) != 0) {
#ifdef _WIN32
                    const int e = WSAGetLastError();
                    std::cout << "listen() failed family=" << addr.FamilyValue() << " wsa=" << e << "\n";
#else
                    const int e = errno;
                    std::cout << "listen() failed family=" << addr.FamilyValue()
                        << " errno=" << e << " " << std::strerror(e) << "\n";
#endif
                    CloseSocket(fd);
                    continue;
                }

                SetNonBlocking(fd);
                m_listenFds.push_back(fd);
            }
            ::freeaddrinfo(result);
            if (m_listenFds.empty()) return false;
            return true;
        }

        int Server::SetNonBlocking(SocketType fdOrSocket) {
#ifdef _WIN32
            u_long mode = 1;
            return ::ioctlsocket(fdOrSocket, FIONBIO, &mode);
#else
            int flags = ::fcntl((int)fdOrSocket, F_GETFL, 0);
            if (flags < 0) return -1;
            return ::fcntl((int)fdOrSocket, F_SETFL, flags | O_NONBLOCK);
#endif
        }

        void Server::SetStatus(Status status) {
            m_status.store(status, std::memory_order_release);
            m_stateCv.notify_all();
        }

        bool Server::StatusEquals(Status status) const noexcept {
            return m_status.load(std::memory_order_acquire) == status;
        }
    }
}
