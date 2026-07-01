#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace LikesProgram {
    namespace Net {
        class EventLoop;

        class LIKESPROGRAM_NET_API Poller {
        public:
            using FdKey = std::uintptr_t;

            // 创建未绑定 EventLoop 的轮询器。
            Poller();
            // 创建绑定指定 EventLoop 的轮询器。
            explicit Poller(EventLoop* ownerLoop);
            virtual ~Poller();

            Poller(const Poller&) = delete;
            Poller& operator=(const Poller&) = delete;

            // 设置所属 EventLoop。
            void SetEventLoop(EventLoop* ownerLoop) noexcept;
            // 添加 Channel。
            virtual bool AddChannel(Channel* channel) = 0;
            // 删除 Channel。
            virtual bool RemoveChannel(Channel* channel) = 0;
            // 更新 Channel 关注事件。
            virtual bool UpdateChannel(Channel* channel) = 0;
            // 等待就绪事件并填充 active 容器。
            virtual void Poll(int timeoutMs, std::vector<Channel*>& active) = 0;
            // 查询轮询器是否持有指定 Channel。
            bool HasChannel(const Channel* channel) const;
            // 返回最近一次系统错误码。
            int LastError() const noexcept;

        protected:
            // 将 socket 转为 unordered_map 稳定 key。
            static FdKey ToKey(SocketType fd) noexcept;
            // 保存最近一次系统错误码。
            void SetLastError(int error) noexcept;
            // 保存 Channel 到内部 fd 映射。
            bool StoreChannel(Channel* channel);
            // 从内部 fd 映射删除 Channel。
            bool EraseChannel(Channel* channel);
            // 查找内部 fd 映射中的 Channel。
            Channel* FindStoredChannel(SocketType fd) const;
            // 返回内部 fd 映射是否为空。
            bool StoredChannelsEmpty() const noexcept;
            // 返回内部 fd 映射快照，避免派生类暴露 unordered_map。
            std::vector<Channel*> StoredChannelsSnapshot() const;

        private:
            struct PollerImpl;

            PollerImpl* m_impl = nullptr;                         // 轮询器实现，避免导出类携带 STL 容器
        };

        // 创建当前平台默认轮询器，Net 基础包不引入 TLS 或第三方依赖。
        LIKESPROGRAM_NET_API std::unique_ptr<Poller> CreateDefaultPoller(EventLoop* ownerLoop);
    }
}
