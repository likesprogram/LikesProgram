#pragma once
#include "IOEvent.hpp"
#include <memory>
#include <atomic>
#ifdef _WIN32
#include <winsock2.h>
using socklen_t = int;
using SocketType = SOCKET;
#else
using SocketType = int;
#endif

namespace LikesProgram {
	namespace Server {
        class Connection; // 前向声明

        class Channel {
        public:
            /// <summary>
            /// 构造函数
            /// </summary>
            /// <param name="fd">文件描述符</param>
            /// <param name="events">初始关注的事件集合（读/写/关闭等）</param>
            /// <param name="connection">Channel 对应的 Connection </param>
            Channel(int fd, IOEvent events, std::shared_ptr<Connection> connection)
                : m_fd(fd), m_events(static_cast<int>(events)), m_connection(connection), m_writeEnabled(false) {
                m_connection->SetChannel(this);
            }

            // 启用或禁用某个事件（读/写/关闭/超时）
            void EnableEvent(IOEvent event, bool flag) {
                if (flag) m_events |= static_cast<int>(event);
                else m_events &= ~static_cast<int>(event);
            }
            // 判断某个事件是否已启用
            bool IsEventEnabled(IOEvent event) const { return (m_events & static_cast<int>(event)) != 0; }

            // 写事件单独开关（通常用于非阻塞写队列）
            void EnableWrite(bool flag) { m_writeEnabled.store(flag, std::memory_order_relaxed); }
            // 判断写事件是否启用
            bool IsWriteEnabled() const { return m_writeEnabled.load(std::memory_order_relaxed); }

            // 获取文件描述符，用于 Poller 或底层 I/O
            int GetSocket() const { return m_fd; }

            // 获取关联的 Connection 对象
            Connection* GetConnection() const { return m_connection.get(); }

        private:
            int m_fd;                           // 连接 fd
            int m_events;                       // 当前关注事件
            std::shared_ptr<Connection> m_connection; // 类型安全指向 Connection
            std::atomic<bool> m_writeEnabled;   // 高性能写事件开关
        };
	}
}