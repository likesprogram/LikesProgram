#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/IOEvent.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <functional>

namespace LikesProgram {
    namespace Net {
        class EventLoop;

        class LIKESPROGRAM_NET_API Channel {
        public:
            enum class Index : int {
                New = 0,
                Added = 1,
                Deleted = 2
            };

            using Callback = std::function<void()>;

            // 绑定一个 socket 与所属 EventLoop。
            Channel(EventLoop* loop, SocketType fd, IOEvent events = IOEvent::None);
            // 释放事件回调与状态缓存。
            ~Channel();

            Channel(const Channel&) = delete;
            Channel& operator=(const Channel&) = delete;

            // 返回当前关注事件。
            IOEvent Events() const noexcept;
            // 启用指定事件并通知 EventLoop 更新轮询器。
            void Enable(IOEvent event);
            // 禁用指定事件并通知 EventLoop 更新轮询器。
            void Disable(IOEvent event);
            // 禁用全部事件，关闭前应优先调用。
            void DisableAll();
            // 启用读事件。
            void EnableReading();
            // 禁用读事件。
            void DisableReading();
            // 启用写事件。
            void EnableWriting();
            // 禁用写事件。
            void DisableWriting();
            // 判断指定事件是否已关注。
            bool IsEventEnabled(IOEvent event) const noexcept;

            // 返回底层 socket。
            SocketType GetSocket() const noexcept;
            // 设置本轮就绪事件。
            void SetRevents(IOEvent event) noexcept;
            // 返回本轮就绪事件。
            IOEvent Revents() const noexcept;
            // Reactor 分发入口。
            void HandleEvent();

            // 设置读事件回调。
            void SetReadCallback(Callback callback);
            // 设置写事件回调。
            void SetWriteCallback(Callback callback);
            // 设置关闭事件回调。
            void SetCloseCallback(Callback callback);
            // 设置错误事件回调。
            void SetErrorCallback(Callback callback);

            // 返回 Poller 中的状态机索引。
            Index GetIndex() const noexcept;
            // 更新 Poller 中的状态机索引。
            void SetIndex(Index index) noexcept;

        private:
            struct ChannelImpl;

            // 事件集合变化后通知所属 EventLoop。
            void UpdateLoopChannel(IOEvent oldEvents);

            ChannelImpl* m_impl = nullptr;           // 事件通道实现，避免导出类携带 std::function
        };
    }
}
