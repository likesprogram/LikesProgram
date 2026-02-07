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
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include "../../../include/LikesProgram/net/pollers/EpollPoller.hpp"
#endif
#include <stdexcept>
#include <string>

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

        Server::Server(unsigned short port, PollerFactory pollerFactory, ConnectionFactory connectionFactory, size_t subLoopCount) : m_port(port) {
#ifdef _WIN32
            (void)EnsureWinsock();
#endif

            // 监听 socket
            if (Listen() == (SocketType)-1) {
                throw std::runtime_error("Failed to listen on port " + std::to_string(port));
            }

            // 创建主事件循环
            m_mainLoop = std::make_shared<MainEventLoop>(
                std::move(pollerFactory),
                std::move(connectionFactory),
                subLoopCount
            );

            // 将监听通道注册到主循环中
            m_listenChannel = std::make_unique<Channel>(
                m_mainLoop.get(),
                m_listenFd,
                IOEvent::Read,
                nullptr
            );
            m_mainLoop->RegisterChannel(m_listenChannel.get());
        }

        Server::~Server() {
            Stop();
            if (m_mainLoop && m_listenChannel) {
                m_mainLoop->UnregisterChannel(m_listenChannel.get());
                m_listenChannel.reset();
            }

            if (m_listenFd != kInvalidSocket) {
                CloseSocket(m_listenFd);
                m_listenFd = kInvalidSocket;
            }
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

        SocketType Server::Listen() {
#ifdef _WIN32
            SOCKET listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listenSock == INVALID_SOCKET) return (SocketType)-1;

            char opt = 1;
            ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(m_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (::bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                CloseSocket(listenSock);
                return (SocketType)-1;
            }
            if (::listen(listenSock, 128) == SOCKET_ERROR) {
                CloseSocket(listenSock);
                return (SocketType)-1;
            }

            m_listenFd = listenSock;
#else
            m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (m_listenFd < 0) return (SocketType)-1;

            int opt = 1;
            ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(m_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (::bind(m_listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
                CloseSocket(m_listenFd);
                m_listenFd = (SocketType)-1;
                return (SocketType)-1;
            }
            if (::listen(m_listenFd, 128) < 0) {
                CloseSocket(m_listenFd);
                m_listenFd = (SocketType)-1;
                return (SocketType)-1;
            }
#endif

            SetNonBlocking(m_listenFd);
            return m_listenFd;
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
