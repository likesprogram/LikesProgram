#pragma once
#ifdef _WIN32
#include "../Poller.hpp"
#include <vector>

namespace LikesProgram {
	namespace Net {
        class WindowsSelectPoller final : public Poller {
        public:
            explicit WindowsSelectPoller(EventLoop* ownerLoop);
            ~WindowsSelectPoller() override = default;

            bool AddChannel(Channel* channel) override;
            bool RemoveChannel(Channel* channel) override;
            bool UpdateChannel(Channel* channel) override;

            void Poll(int timeoutMs, std::vector<Channel*>& active) override;

        private:
            // 将 m_channels 里的 interested events 填入 fd_set（select 会修改，所以每次 Poll 都要重建）
            void BuildPollFds(std::vector<WSAPOLLFD>& fds) const;
            // 将 WSAPoll 的 revents 映射 IOEvent
            static IOEvent ToIOEvent(short revents);
        };
	}
}
#endif
