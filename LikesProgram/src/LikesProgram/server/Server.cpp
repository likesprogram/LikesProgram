#include "../../../include/LikesProgram/server/Server.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace LikesProgram {
    namespace Server {
        Server::Server(unsigned short port, Poller* poller, size_t threadPoolSize) : m_port(port) {
#ifdef _WIN32
            static WinsockInit g_winsockInit;
#endif
            // 创建监听 socket
            if (Listen() == -1) {
                throw std::runtime_error("Failed to listen on port " + std::to_string(port));
            }

            // 创建 MainEventLoop 对象
            m_mainLoop = std::make_shared<MainEventLoop>(poller,
                [this](SocketType fd, EventLoop* eventLoop) {                  // lambda 用于 accept 回调
                    return this->AcceptConnection(fd, eventLoop); // 调用 Server::AcceptConnection
                },
                0, threadPoolSize                            // 线程池大小
            );

            // 创建监听 Channel（用于监听客户端连接）
            Channel* channel = new Channel(m_listenFd, IOEvent::Read, nullptr);
            m_mainLoop->RegisterChannel(channel);   // 注册 Channel 到 MainEventLoop
        }

        std::shared_ptr<Connection> Server::AcceptConnection(SocketType listenFd, EventLoop* eventLoop) {
            struct sockaddr_in clientAddr {}; // 客户端地址结构
            socklen_t addrLen = sizeof(clientAddr); // 地址长度
#ifdef _WIN32
            SOCKET clientFd = accept(listenFd, (sockaddr*)&clientAddr, &addrLen); // Windows accept
            if (clientFd == INVALID_SOCKET) return nullptr; // accept 失败返回 -1
#else
            int clientFd = ::accept(listenSocket, (struct sockaddr*)&clientAddr, &addrLen); // Linux accept
            if (clientFd < 0) return -1; // accept 失败返回 -1
#endif
            SetNonBlocking(clientFd); // 设置客户端 socket 为非阻塞
            return CreateConnection(clientFd, eventLoop); // 返回新的连接对象
        }

        void Server::CloseSocket(int fdOrSocket) {
#ifdef _WIN32
            closesocket(fdOrSocket); // Windows 关闭套接字
#else
            close(fdOrSocket);       // Linux/Unix 关闭套接字
#endif
        }

        Server::~Server() {
            // 析构函数，目前空，子类可扩展清理资源
            CloseSocket(m_listenFd);
        }

        SocketType Server::Listen() {
#ifdef _WIN32
            SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listenSock == INVALID_SOCKET) {
                printf("socket failed: %d\n", WSAGetLastError());
                return -1;
            }

            char opt = 1;
            if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == SOCKET_ERROR) {
                printf("setsockopt failed: %d\n", WSAGetLastError());
                closesocket(listenSock);
                return -1;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(m_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                printf("bind failed: %d\n", WSAGetLastError());
                closesocket(listenSock);
                return -1;
            }

            if (listen(listenSock, 128) == SOCKET_ERROR) {
                printf("listen failed: %d\n", WSAGetLastError());
                closesocket(listenSock);
                return -1;
            }

            m_listenFd = listenSock;
#else
            m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
            if (m_listenFd < 0) {
                perror("socket");
                return -1;
            }

            int opt = 1;
            if (setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                perror("setsockopt");
                close(m_listenFd);
                m_listenFd = -1;
                return -1;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(m_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            if (bind(m_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("bind");
                close(m_listenFd);
                m_listenFd = -1;
                return -1;
            }

            if (listen(m_listenFd, 128) < 0) {
                perror("listen");
                close(m_listenFd);
                m_listenFd = -1;
                return -1;
            }
#endif
            // 设置非阻塞
            SetNonBlocking(m_listenFd);
            return m_listenFd;
        }

        int Server::SetNonBlocking(int fdOrSocket) {
#ifdef _WIN32
            u_long mode = 1;
            return ioctlsocket(fdOrSocket, FIONBIO, &mode); // Windows 非阻塞
#else
            int flags = fcntl(fdOrSocket, F_GETFL, 0);      // 获取当前 flag
            return fcntl(fdOrSocket, F_SETFL, flags | O_NONBLOCK); // 设置非阻塞
#endif
        }
    }
}
