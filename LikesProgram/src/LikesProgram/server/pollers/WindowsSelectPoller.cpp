#ifdef _WIN32
#include "../../../../include/LikesProgram/server/pollers/WindowsSelectPoller.hpp"
#include <algorithm>
#include <stdexcept>

namespace LikesProgram {
	namespace Server {
		WindowsSelectPoller::WindowsSelectPoller() : m_maxFd(0) {
			FD_ZERO(&m_readSet);
			FD_ZERO(&m_writeSet);
			FD_ZERO(&m_exceptSet);
		}

		WindowsSelectPoller::~WindowsSelectPoller() {
		}

		bool WindowsSelectPoller::AddChannel(Channel* channel) {
			int fd = channel->GetSocket();
			m_channels[fd] = channel;

			if (channel->IsEventEnabled(IOEvent::Read))
				FD_SET(fd, &m_readSet);
			if (channel->IsEventEnabled(IOEvent::Write) && channel->IsWriteEnabled())
				FD_SET(fd, &m_writeSet);
			FD_SET(fd, &m_exceptSet);

			m_maxFd = std::max<int>(m_maxFd, fd);
			return true;
		}

        bool WindowsSelectPoller::RemoveChannel(Channel* channel) {
            int fd = channel->GetSocket();
            m_channels.erase(fd);
            FD_CLR(fd, &m_readSet);
            FD_CLR(fd, &m_writeSet);
            FD_CLR(fd, &m_exceptSet);

            if (fd == m_maxFd) {
                // 重新计算最大 fd
                m_maxFd = 0;
                for (auto& kv : m_channels) {
                    m_maxFd = std::max<int>(m_maxFd, static_cast<int>(kv.first));
                }
            }
            return true;
        }

        bool WindowsSelectPoller::UpdateChannel(Channel* channel) {
            // 在 select 中只能修改 fd_set
            int fd = channel->GetSocket();
            FD_CLR(fd, &m_readSet);
            FD_CLR(fd, &m_writeSet);

            if (channel->IsEventEnabled(IOEvent::Read))
                FD_SET(fd, &m_readSet);
            if (channel->IsEventEnabled(IOEvent::Write) && channel->IsWriteEnabled())
                FD_SET(fd, &m_writeSet);

            return true;
        }

        std::vector<Channel*> WindowsSelectPoller::Dispatch(int timeoutMs) {
            std::vector<Channel*> activeChannels;

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            fd_set readSet = m_readSet;
            fd_set writeSet = m_writeSet;
            fd_set exceptSet = m_exceptSet;

            int n = select(m_maxFd + 1, &readSet, &writeSet, &exceptSet, &tv);
            if (n < 0) return activeChannels; // 错误处理可增强

            for (auto& kv : m_channels) {
                int fd = kv.first;
                Channel* ch = kv.second;

                if (FD_ISSET(fd, &readSet) || FD_ISSET(fd, &writeSet) || FD_ISSET(fd, &exceptSet)) {
                    activeChannels.push_back(ch);
                }
            }

            return activeChannels;
        }
	}
}

#endif
