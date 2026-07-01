#pragma once
#include <LikesProgram/Net/Poller.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class SelectPoller final : public Poller {
            public:
                // 创建基于 select 的跨平台基础轮询器。
                explicit SelectPoller(EventLoop* ownerLoop);
                // 添加 Channel 到 fd 映射。
                bool AddChannel(Channel* channel) override;
                // 从 fd 映射移除 Channel。
                bool RemoveChannel(Channel* channel) override;
                // 更新 Channel 关注事件，select 版本只需确认存在。
                bool UpdateChannel(Channel* channel) override;
                // 等待可读/可写/异常事件。
                void Poll(int timeoutMs, std::vector<Channel*>& active) override;
            };
        }
    }
}
