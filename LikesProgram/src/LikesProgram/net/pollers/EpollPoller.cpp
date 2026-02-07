#ifndef _WIN32
#include "../../../../include/LikesProgram/net/pollers/EpollPoller.hpp"
#include "../../../../include/LikesProgram/net/IOEvent.hpp"
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

namespace LikesProgram {
	namespace Net {

        static IOEvent EpollToIOEvent(uint32_t ev) {
            IOEvent out = IOEvent::None;

            if (ev & (EPOLLERR))     out |= IOEvent::Error;
            if (ev & (EPOLLHUP))     out |= IOEvent::Close;
#ifdef EPOLLRDHUP
            if (ev & (EPOLLRDHUP))   out |= IOEvent::Close; // 半关闭（对端关闭写端）
#endif

            if (ev & (EPOLLIN | EPOLLPRI)) out |= IOEvent::Read;
            if (ev & EPOLLOUT)             out |= IOEvent::Write;

            return out;
        }

        static uint32_t IOEventToEpoll(IOEvent ev) {
            uint32_t out = 0;
            if (Has(ev, IOEvent::Read))  out |= (EPOLLIN | EPOLLPRI);
            if (Has(ev, IOEvent::Write)) out |= EPOLLOUT;

            // 错误与挂起
            out |= EPOLLERR | EPOLLHUP;
#ifdef EPOLLRDHUP
            out |= EPOLLRDHUP;
#endif
            return out;
        }

        EpollPoller::EpollPoller(EventLoop* ownerLoop)
            : Poller(ownerLoop),
            m_events(64) {
            m_epollfd = ::epoll_create1(EPOLL_CLOEXEC);
            if (m_epollfd < 0) {
                SetLastError(errno);
                // throw 或 assert
            }
        }

        EpollPoller::~EpollPoller() {
            if (m_epollfd >= 0) {
                ::close(m_epollfd);
                m_epollfd = -1;
            }
        }

        bool EpollPoller::UpdateImpl(int op, Channel* channel) {
            if (!channel) return false;

            struct epoll_event e;
            e.events = IOEventToEpoll(channel->Events());
            e.data.ptr = channel;

            const int fd = (int)channel->GetSocket();
            if (::epoll_ctl(m_epollfd, op, fd, &e) < 0) {
                SetLastError(errno);
                return false;
            }
            return true;
        }

        bool EpollPoller::AddChannel(Channel* channel) {
            if (!channel) return false;

            const auto key = ToKey(channel->GetSocket());
            m_channels[key] = channel;

            if (!UpdateImpl(EPOLL_CTL_ADD, channel)) return false;

            channel->SetIndex(Channel::Index::Added);
            return true;
        }

        bool EpollPoller::RemoveChannel(Channel* channel) {
            if (!channel) return false;

            const auto key = ToKey(channel->GetSocket());
            m_channels.erase(key);

            // 删除时就算失败（比如重复 DEL）不致命
            (void)UpdateImpl(EPOLL_CTL_DEL, channel);

            channel->SetIndex(Channel::Index::Deleted);
            return true;
        }

        bool EpollPoller::UpdateChannel(Channel* channel) {
            if (!channel) return false;

            const auto idx = channel->GetIndex();
            const bool noEvents = (channel->Events() == IOEvent::None);
            const auto key = ToKey(channel->GetSocket());

            if (idx == Channel::Index::New || idx == Channel::Index::Deleted) {
                if (noEvents) {
                    channel->SetIndex(Channel::Index::Deleted);
                    return true;
                }

                if (!UpdateImpl(EPOLL_CTL_ADD, channel)) return false;

                m_channels[key] = channel;
                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            // idx == Added
            if (noEvents) {
                const int fd = (int)channel->GetSocket();

                if (::epoll_ctl(m_epollfd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
                    const int e = errno;
                    // 重复删除/已关闭 fd：允许继续逻辑删除，避免关闭流程卡死
                    if (e != ENOENT && e != EBADF) {
                        SetLastError(e);
                        return false;
                    }
                }

                channel->SetIndex(Channel::Index::Deleted);
                m_channels.erase(key);
                return true;
            }

            return UpdateImpl(EPOLL_CTL_MOD, channel);
        }

        void EpollPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
            active.clear();

            const int n = ::epoll_wait(m_epollfd, m_events.data(), (int)m_events.size(), timeoutMs);
            if (n < 0) {
                if (errno == EINTR) return;
                SetLastError(errno);
                return;
            }

            for (int i = 0; i < n; ++i) {
                auto* ch = static_cast<Channel*>(m_events[i].data.ptr);
                if (!ch) continue;

                ch->SetRevents(EpollToIOEvent(m_events[i].events));
                active.push_back(ch);
            }

            // 扩容：高并发瞬时事件多时减少下次 realloc
            if ((size_t)n == m_events.size()) {
                m_events.resize(m_events.size() * 2);
            }
        }
	}
}
#endif
