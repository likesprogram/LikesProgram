#pragma once
#ifndef _WIN32
#include "../Poller.hpp"
#include <vector>

namespace LikesProgram {
	namespace Net {
        class EpollPoller final : public Poller {
        public:
            explicit EpollPoller(EventLoop* ownerLoop);
            ~EpollPoller() override;

            bool AddChannel(Channel* channel) override;
            bool RemoveChannel(Channel* channel) override;
            bool UpdateChannel(Channel* channel) override;
            void Poll(int timeoutMs, std::vector<Channel*>& active) override;

        private:
            bool UpdateImpl(int op, Channel* channel);

        private:
            int m_epollfd = -1;
            std::vector<struct epoll_event> m_events;
        };
	}
}
#endif
