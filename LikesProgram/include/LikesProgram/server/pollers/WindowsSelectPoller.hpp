#pragma once
#include "../Poller.hpp"
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace LikesProgram {
	namespace Server {
#ifdef _WIN32
		class WindowsSelectPoller : public Poller {
		public:
			WindowsSelectPoller();
			~WindowsSelectPoller() override;
			// 添加
			bool AddChannel(Channel* channel) override;
			// 删除
			bool RemoveChannel(Channel* channel) override;
			// 修改
			bool UpdateChannel(Channel* channel) override;
			// 事件监测
			std::vector<Channel*> Dispatch(int timeoutMs = 2) override;
		private:
			fd_set m_readSet;
			fd_set m_writeSet;
			fd_set m_exceptSet;
			int m_maxFd;
		};
#endif
	}
}
