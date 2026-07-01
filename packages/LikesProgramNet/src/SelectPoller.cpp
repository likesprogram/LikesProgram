#include "net/SelectPoller.hpp"
#include "net/SocketOps.hpp"
#include <LikesProgram/Net/EventLoop.hpp>
#include <algorithm>
#include <chrono>
#include <thread>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            SelectPoller::SelectPoller(EventLoop* ownerLoop)
                : Poller(ownerLoop) {
            }

            bool SelectPoller::AddChannel(Channel* channel) {
                if (channel == nullptr || channel->GetSocket() == kInvalidSocket) return false;

                if (!StoreChannel(channel)) return false;
                channel->SetIndex(Channel::Index::Added);
                return true;
            }

            bool SelectPoller::RemoveChannel(Channel* channel) {
                if (channel == nullptr) return false;

                const bool erased = EraseChannel(channel); // 通过基类隐藏的 fd 映射删除
                channel->SetIndex(Channel::Index::Deleted);
                return erased;
            }

            bool SelectPoller::UpdateChannel(Channel* channel) {
                if (channel == nullptr || channel->GetSocket() == kInvalidSocket) return false;

                if (FindStoredChannel(channel->GetSocket()) == nullptr) return AddChannel(channel);
                return true;
            }

            void SelectPoller::Poll(int timeoutMs, std::vector<Channel*>& active) {
                active.clear();
                const std::vector<Channel*> channels = StoredChannelsSnapshot(); // 固定本轮 fd 映射快照
                if (channels.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeoutMs, 0)));
                    return;
                }

                fd_set readSet; // select 读事件集合
                fd_set writeSet; // select 写事件集合
                fd_set errorSet; // select 异常事件集合
                FD_ZERO(&readSet);
                FD_ZERO(&writeSet);
                FD_ZERO(&errorSet);

                SocketType maxFd = 0; // POSIX select 的 nfds 基准，Windows 会忽略
                bool hasObservedFd = false; // 是否至少存在一个关注了事件的 fd
                for (Channel* channel : channels) {
                    if (channel == nullptr) continue;

                    const SocketType fd = channel->GetSocket();
                    const IOEvent events = channel->Events();
                    if (events == IOEvent::None) continue;
                    hasObservedFd = true;
                    if (HasEvent(events, IOEvent::Read)) FD_SET(fd, &readSet);
                    if (HasEvent(events, IOEvent::Write)) FD_SET(fd, &writeSet);
                    FD_SET(fd, &errorSet);
                    if (fd > maxFd) maxFd = fd;
                }

                if (!hasObservedFd) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeoutMs, 0)));
                    return;
                }

                timeval timeout{}; // select 超时对象，单位拆成秒和微秒
                timeout.tv_sec = timeoutMs <= 0 ? 0 : timeoutMs / 1000;
                timeout.tv_usec = timeoutMs <= 0 ? 0 : (timeoutMs % 1000) * 1000;

                const int rc = ::select(
#ifdef _WIN32
                    0,
#else
                    static_cast<int>(maxFd + 1),
#endif
                    &readSet, &writeSet, &errorSet, &timeout);

                if (rc < 0) {
                    const int error = GetLastSocketError(); // 保存本轮 select 错误
                    if (!IsInterrupted(error)) SetLastError(error);
                    return;
                }

                if (rc == 0) return;

                for (Channel* channel : channels) {
                    if (channel == nullptr) continue;

                    IOEvent revents = IOEvent::None; // 本轮就绪事件集合
                    const SocketType fd = channel->GetSocket();
                    if (FD_ISSET(fd, &readSet)) revents |= IOEvent::Read;
                    if (FD_ISSET(fd, &writeSet)) revents |= IOEvent::Write;
                    if (FD_ISSET(fd, &errorSet)) revents |= IOEvent::Error;

                    if (revents != IOEvent::None) {
                        channel->SetRevents(revents);
                        active.push_back(channel);
                    }
                }
            }
        }

        std::unique_ptr<Poller> CreateDefaultPoller(EventLoop* ownerLoop) {
            return std::make_unique<Internal::SelectPoller>(ownerLoop);
        }
    }
}
