#pragma once
#include "IOEvent.hpp"
#include "SocketType.hpp"
#include <memory>
#include <atomic>

namespace LikesProgram {
	namespace Net {
        class Connection; // 前向声明
        class EventLoop; // 前向声明

        class Channel {
        public:
            enum class Index : int {
                New = 0,
                Added = 1,
                Deleted = 2
            };
            /// <summary>
            /// 构造函数
            /// </summary>
            /// <param name="fd">文件描述符</param>
            /// <param name="events">初始关注的事件集合（读/写/关闭等）</param>
            /// <param name="connection">Channel 对应的 Connection </param>
            Channel(EventLoop* loop, SocketType fd, IOEvent events, Connection* connection);

            // 当前关注事件
            IOEvent Events() const;

            // 启用某个事件（读/写/关闭/超时）
            void Enable(IOEvent event);

            // 禁用某个事件（读/写/关闭/超时）
            void Disable(IOEvent event);

            // 全部禁用（关闭前必须做，避免 close 后仍触发）
            void DisableAll();

            // 启用读事件
            void EnableReading();
            // 禁用读事件
            void DisableReading();
            // 启用写事件
            void EnableWriting();
            // 禁用写事件
            void DisableWriting();

            // 判断某个事件是否已启用
            bool IsEventEnabled(IOEvent event) const;

            // 获取文件描述符，用于 Poller 或底层 I/O
            SocketType GetSocket() const;

            // 就绪事件
            void SetRevents(IOEvent event);
            IOEvent Revents() const;

            // 获取关联的 Connection 对象
            Connection* GetConnection() const;
            void SetConnection(Connection* c) noexcept;

            // Reactor 分发入口：EventLoop 拿到 active channel 后调用
            void HandleEvent();
            Index GetIndex() const noexcept;
            void SetIndex(Index idx) noexcept;
        private:
            EventLoop* m_loop = nullptr;                    // owner
            SocketType m_fd = (SocketType)-1;               // 连接 fd
            IOEvent m_events = IOEvent::None;               // 当前关注事件
            IOEvent m_revents = IOEvent::None;              // 就绪事件
            Connection* m_connection = nullptr;             // 指向 Connection
            Index m_index = Index::New;

            void UpdateLoopChannel(IOEvent oldEvent);
        };
	}
}