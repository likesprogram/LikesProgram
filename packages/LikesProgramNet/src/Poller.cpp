#include <LikesProgram/Net/Poller.hpp>
#include <unordered_map>

namespace LikesProgram {
    namespace Net {
        struct Poller::PollerImpl {
            EventLoop* m_ownerLoop = nullptr;              // 所属 EventLoop，不拥有生命周期
            std::unordered_map<FdKey, Channel*> m_channels; // fd 到 Channel 的活动映射
            int m_lastError = 0;                           // 最近一次轮询器错误码
        };

        Poller::Poller()
            : m_impl(new PollerImpl{}) {
        }

        Poller::Poller(EventLoop* ownerLoop)
            : m_impl(new PollerImpl{}) {
            m_impl->m_ownerLoop = ownerLoop;
        }

        Poller::~Poller() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Poller::SetEventLoop(EventLoop* ownerLoop) noexcept {
            if (m_impl) m_impl->m_ownerLoop = ownerLoop;
        }

        bool Poller::HasChannel(const Channel* channel) const {
            if (channel == nullptr) return false;

            return FindStoredChannel(channel->GetSocket()) == channel;
        }

        int Poller::LastError() const noexcept {
            return m_impl ? m_impl->m_lastError : 0;
        }

        Poller::FdKey Poller::ToKey(SocketType fd) noexcept {
            return static_cast<FdKey>(fd);
        }

        void Poller::SetLastError(int error) noexcept {
            if (m_impl) m_impl->m_lastError = error;
        }

        bool Poller::StoreChannel(Channel* channel) {
            if (m_impl == nullptr || channel == nullptr) return false;

            const FdKey key = ToKey(channel->GetSocket()); // 内部映射使用 uintptr_t 稳定 key
            return m_impl->m_channels.emplace(key, channel).second;
        }

        bool Poller::EraseChannel(Channel* channel) {
            if (m_impl == nullptr || channel == nullptr) return false;

            const FdKey key = ToKey(channel->GetSocket()); // 删除时按 fd 精确定位
            return m_impl->m_channels.erase(key) != 0;
        }

        Channel* Poller::FindStoredChannel(SocketType fd) const {
            if (m_impl == nullptr) return nullptr;

            const FdKey key = ToKey(fd); // 查询时保持与插入一致的 key 规则
            const auto it = m_impl->m_channels.find(key);
            return it == m_impl->m_channels.end() ? nullptr : it->second;
        }

        bool Poller::StoredChannelsEmpty() const noexcept {
            return m_impl == nullptr || m_impl->m_channels.empty();
        }

        std::vector<Channel*> Poller::StoredChannelsSnapshot() const {
            std::vector<Channel*> channels; // 派生轮询器只需要 Channel 快照
            if (m_impl == nullptr) return channels;

            channels.reserve(m_impl->m_channels.size());
            for (const auto& item : m_impl->m_channels) {
                channels.push_back(item.second);
            }
            return channels;
        }
    }
}
