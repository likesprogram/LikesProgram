#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "EventLoop.hpp"
#include "Connection.hpp"
#include "Channel.hpp"
#include "Transport.hpp"

namespace LikesProgram {
    namespace Server {
        // 跨平台 Winsock 初始化
#ifdef _WIN32
        struct WinsockInit {
            WinsockInit() {
                std::call_once(initFlag, []() {
                    WSADATA wsaData;
                    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                        throw std::runtime_error("WSAStartup failed");
                    }
                    });
            }
            ~WinsockInit() {
                WSACleanup();
            }
            static std::once_flag initFlag;
        };
        std::once_flag WinsockInit::initFlag;
#endif
        // Server 基类
        class Server {
        public:
            // 构造函数，创建监听 Channel 并注册到 MainEventLoop
            explicit Server(unsigned short port, Poller* poller, size_t threadPoolSize = 0);

            // 必须重写：创建自定义 Connection 对象
            virtual std::shared_ptr<Connection> CreateConnection(SocketType fd, EventLoop* eventLoop) = 0;

            // 初始化 TLS（可选），子类可重写实现
            virtual void InitializeTls() {}

            // accept 新连接函数
            std::shared_ptr<Connection> AcceptConnection(SocketType listenFd, EventLoop* eventLoop);

            // 关闭 socket 函数，跨平台封装
            void CloseSocket(int fdOrSocket);

            virtual ~Server();

        private:

            // 创建监听 socket
            SocketType Listen();

            // 设置 socket 为非阻塞模式
            int SetNonBlocking(int fdOrSocket);

        protected:
            std::shared_ptr<MainEventLoop> m_mainLoop; // 主事件循环对象
            SocketType m_listenFd;                     // 监听 socket fd
            unsigned short m_port;                     // 监听端口号
        };
    }
}
