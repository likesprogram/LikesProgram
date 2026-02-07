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

        void WindowsSelectPoller::BuildFdSets(fd_set& readfds, fd_set& writefds, fd_set& exceptfds) const {
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_ZERO(&exceptfds);

            // select 模型：每轮都从 channel map 重建 fd_set（O(n)）
            for (const auto& kv : m_channels) {
                Channel* ch = kv.second;
                if (!ch) continue;

                const SocketType fd = ch->GetSocket();
                const IOEvent ev = ch->Events();

                if (ev == IOEvent::None) continue;

                if ((ev & IOEvent::Read) != IOEvent::None) FD_SET(fd, &readfds);

                if ((ev & IOEvent::Write) != IOEvent::None) FD_SET(fd, &writefds);

                // exceptfds 用于异常/错误（连接 reset 等）
                FD_SET(fd, &exceptfds);
            }
        }

        bool WindowsSelectPoller::AddChannel(Channel* channel) {
            if (!channel) return false;

            const auto key = ToKey(channel->GetSocket());

            // 没有关注事件：不必加入（保持 Deleted）
            if (channel->Events() == IOEvent::None) {
                channel->SetIndex(Channel::Index::Deleted);
                return true;
            }

            // FD_SETSIZE 限制：select 可监控的 fd 数量有限
            if (m_channels.size() >= FD_SETSIZE) {
                SetLastError(WSAEINVAL);
                return false;
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
                // 加入
                if (m_channels.size() >= FD_SETSIZE) {
                    SetLastError(WSAEINVAL);
                    return false;
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

            fd_set readfds, writefds, exceptfds;
            BuildFdSets(readfds, writefds, exceptfds);

            // select 的超时
            timeval tv;
            tv.tv_sec = (timeoutMs < 0) ? 0 : timeoutMs / 1000;
            tv.tv_usec = (timeoutMs < 0) ? 0 : (timeoutMs % 1000) * 1000;

            // Windows 下 nfds 参数被忽略，传 0 即可
            int n = ::select(0, &readfds, &writefds, &exceptfds, (timeoutMs < 0 ? nullptr : &tv));
            if (n < 0) {
                const int e = GetSockErr();
                // WSAEINTR: 被中断，视为本轮无事件
                if (e != WSAEINTR) SetLastError(e);
                return;
            }
            if (n == 0) {
                // timeout
                return;
            }

            // 从所有 channel 中找出命中的（O(n)）
            for (const auto& kv : m_channels) {
                Channel* ch = kv.second;
                if (!ch) continue;

                const SocketType fd = ch->GetSocket();
                IOEvent re = IOEvent::None;

                if (FD_ISSET(fd, &exceptfds)) re |= IOEvent::Error;
                if (FD_ISSET(fd, &readfds))   re |= IOEvent::Read;
                if (FD_ISSET(fd, &writefds))  re |= IOEvent::Write;

                if (re != IOEvent::None) {
                    ch->SetRevents(re);
                    active.push_back(ch);
                }
            }
        }
	}
}
#endif
