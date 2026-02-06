#pragma once
#include "../Poller.hpp"
#ifdef __linux__
#include <sys/epoll.h>
#endif

namespace LikesProgram {
	namespace Server {
#ifdef __linux__
		class EpollPoller : public Poller {
		public:
			EpollPoller();
			~EpollPoller() override;
			// 添加
			bool AddChannel(Channel* channel) override;
			// 删除
			bool RemoveChannel(Channel* channel) override;
			// 修改
			bool UpdateChannel(Channel* channel) override;
			// 事件监测
			std::vector<Channel*> Dispatch(int timeoutMs = 2) override;
		};
#endif
	}
}
