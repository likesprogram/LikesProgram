#ifdef _WIN32
#include "../../../../include/LikesProgram/net/pollers/WindowsSelectPoller.hpp"
#include "../../../../include/LikesProgram/net/IOEvent.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>

namespace LikesProgram {
	namespace Net {
        static int  GetSockErr() { return ::WSAGetLastError(); }

        WindowsSelectPoller::WindowsSelectPoller(EventLoop* ownerLoop)
            : Poller(ownerLoop) {
        }

        void WindowsSelectPoller::BuildPollFds(std::vector<WSAPOLLFD>& fds) const {
            fds.clear();
            fds.reserve(m_channels.size());

            for (const auto& kv : m_channels) {
                Channel* ch = kv.second;
                if (!ch) continue;

                const IOEvent ev = ch->Events();
                if (ev == IOEvent::None) continue;

                WSAPOLLFD pfd{};
                pfd.fd = ch->GetSocket();
                pfd.events = 0;
                pfd.revents = 0;

                // 映射：Read / Write
                if ((ev & IOEvent::Read) != IOEvent::None) {
                    // POLLRDNORM: 普通数据可读
                    pfd.events |= POLLRDNORM;
                    // 也可以额外订阅：POLLPRI（OOB）按需
                }
                if ((ev & IOEvent::Write) != IOEvent::None) {
                    // POLLWRNORM: 普通可写
                    pfd.events |= POLLWRNORM;
                }

                // 错误/挂起类事件通常会在 revents 里返回，无需显式订阅
                fds.push_back(pfd);
            }
        }
        IOEvent WindowsSelectPoller::ToIOEvent(short revents) {
            IOEvent ev = IOEvent::None;

            // 错误：POLLERR / POLLNVAL
            if (revents & (POLLERR | POLLNVAL)) {
                ev |= IOEvent::Error;
            }

            // 挂起：POLLHUP（对端关闭/半关闭等情况）
            if (revents & POLLHUP) {
                ev |= IOEvent::Close;
            }

            // 可读
            if (revents & (POLLRDNORM | POLLRDBAND | POLLPRI)) {
                ev |= IOEvent::Read;
            }

            // 可写
            if (revents & (POLLWRNORM | POLLWRBAND)) {
                ev |= IOEvent::Write;
            }

            return ev;
        }

        bool WindowsSelectPoller::AddChannel(Channel* channel) {
            if (!channel) return false;

            const auto key = ToKey(channel->GetSocket());

            // 没有关注事件：不必加入（保持 Deleted）
            if (channel->Events() == IOEvent::None) {
                channel->SetIndex(Channel::Index::Deleted);
                return true;
            }

            m_channels[key] = channel;
            channel->SetIndex(Channel::Index::Added);
            return true;
        }

        bool WindowsSelectPoller::RemoveChannel(Channel* channel) {
            if (!channel) return false;

            const auto key = ToKey(channel->GetSocket());
            m_channels.erase(key);

            channel->SetIndex(Channel::Index::Deleted);
            return true;
        }

        bool WindowsSelectPoller::UpdateChannel(Channel* channel) {
            if (!channel) return false;

            const auto idx = channel->GetIndex();
            const bool noEvents = (channel->Events() == IOEvent::None);
            const auto key = ToKey(channel->GetSocket());

            // 状态机对齐 epoll 版：
            // New/Deleted + 有事件 => Added（加入 map）
            // Added + 有事件 => 维持 Added（map 已有，select 每轮重建 fd_set）
            // Added + 无事件 => Deleted（从 map 删除）
            if (idx == Channel::Index::New || idx == Channel::Index::Deleted) {
                if (noEvents) {
                    channel->SetIndex(Channel::Index::Deleted);
                    return true;
                }
                m_channels[key] = channel;
                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            // idx == Added
            if (noEvents) {
                m_channels.erase(key);
                channel->SetIndex(Channel::Index::Deleted);
                return true;
            }

            // 仍然 Added：select 每轮 Poll 重建 fd_set，所以不需要做系统调用
            m_channels[key] = channel;
            return true;
        }

        void WindowsSelectPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
            active.clear();

            std::vector<WSAPOLLFD> fds;
            BuildPollFds(fds);

            if (fds.empty()) {
                // 没有 fd 可等：按 timeoutMs 休眠，避免空转
                if (timeoutMs < 0) {
                    ::Sleep(INFINITE);
                }
                else if (timeoutMs > 0) {
                    ::Sleep(static_cast<DWORD>(timeoutMs));
                }
                return;
            }

            // WSAPoll: timeoutMs < 0 表示无限等待（与 poll 类似）
            int n = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
            if (n < 0) {
                const int e = GetSockErr();
                if (e != WSAEINTR) SetLastError(e);
                return;
            }
            if (n == 0) {
                // timeout
                return;
            }


            // 扫描 revents，把命中的 Channel 填回 active
            // 这里用 socket->key 映射去找 Channel，避免额外 vector 索引维护
            for (const auto& pfd : fds) {
                if (pfd.revents == 0) continue;

                const auto key = ToKey(pfd.fd);
                auto it = m_channels.find(key);
                if (it == m_channels.end() || !it->second) continue;

                Channel* ch = it->second;
                const IOEvent re = ToIOEvent(pfd.revents);

                if (re != IOEvent::None) {
                    ch->SetRevents(re);
                    active.push_back(ch);
                }
            }
        }
	}
}
#endif
