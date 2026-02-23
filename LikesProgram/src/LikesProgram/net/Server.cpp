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

        Server::Server(std::vector<String>& addrs, unsigned short port, ConnectionFactory connectionFactory, size_t subLoopCount)
        : Server(addrs, port, DefaultPollerFactory(), std::move(connectionFactory), subLoopCount){ }

        Server::Server(std::vector<String>& addrs, unsigned short port, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount) : m_port(port) {
#ifdef _WIN32
            (void)EnsureWinsock();
#endif

            // 监听 socket
            if (!Listen(addrs)) {
                throw std::runtime_error("Failed to listen on port " + std::to_string(port));
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
            Stop();
            if (m_mainLoop) {
                for (auto& ch : m_listenChannels) {
                    if (ch) m_mainLoop->UnregisterChannel(ch.get());
                }
            }
            m_listenChannels.clear(); // 释放 Channel


            for (auto fd : m_listenFds) if (fd != kInvalidSocket) CloseSocket(fd);
            m_listenFds.clear();
        }

        void Server::Run() {
            if (!m_mainLoop) return;
            m_mainLoop->StartSubLoops();
            m_mainLoop->Run(); // 阻塞
        }

        void Server::Stop() {
            if (!m_mainLoop) return;
            m_mainLoop->Stop();
            m_mainLoop->StopSubLoops();
        }

        std::shared_ptr<Broadcast> Server::GetBroadcast() noexcept {
            return m_mainLoop->GetBroadcast();
        }

        bool Server::Listen(std::vector<String>& addrs) {
            for (auto fd: m_listenFds) if (fd != kInvalidSocket) CloseSocket(fd);
            m_listenFds.clear();

            // 端口字符串
            const std::string portStr = std::to_string(m_port);

            // 若配置为空：默认 "*"
            std::vector<LikesProgram::String> targets = addrs;
            if (targets.empty()) targets.push_back(u"*");

            for (const auto& a : targets) {
                const std::string ip = a.ToStdString();

                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;     // IPv4 + IPv6
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_protocol = IPPROTO_TCP;
                hints.ai_flags = AI_PASSIVE;    // host 为 nullptr 时表示 any

                const char* host = nullptr;
                if (!(ip.empty() || ip == "*")) {
                    host = ip.c_str();
                    hints.ai_flags = 0; // 指定地址时不需要 AI_PASSIVE
                }

                addrinfo* result = nullptr;
                const int gai = ::getaddrinfo(host, portStr.c_str(), &hints, &result);
                if (gai != 0 || !result) continue; // 这个地址解析失败，尝试下一个

                for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
                    SocketType fd = (SocketType)::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                    if (fd == kInvalidSocket) {
#ifdef _WIN32
                        std::cout << "socket() failed family=" << rp->ai_family
                            << " wsa=" << WSAGetLastError() << "\n";
#else
                        std::cout << "socket() failed family=" << rp->ai_family
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
                    if (rp->ai_family == AF_INET6) {
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
                    if (::bind(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) != 0) {
#ifdef _WIN32
                        const int e = WSAGetLastError();
                        std::cout << "bind() failed family=" << rp->ai_family << " wsa=" << e << "\n";
#else
                        const int e = errno;
                        std::cout << "bind() failed family=" << rp->ai_family
                            << " errno=" << e << " " << std::strerror(e) << "\n";
#endif
                        CloseSocket(fd);
                        continue;
                    }

                    if (::listen(fd, 128) != 0) {
#ifdef _WIN32
                        const int e = WSAGetLastError();
                        std::cout << "listen() failed family=" << rp->ai_family << " wsa=" << e << "\n";
#else
                        const int e = errno;
                        std::cout << "listen() failed family=" << rp->ai_family
                            << " errno=" << e << " " << std::strerror(e) << "\n";
#endif
                        CloseSocket(fd);
                        continue;
                    }

                    SetNonBlocking(fd);
                    m_listenFds.push_back(fd);
                }
                ::freeaddrinfo(result);
            }
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
    }
}
