#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/EventLoop.hpp>

namespace LikesProgram {
    namespace Net {
        struct Channel::ChannelImpl {
            EventLoop* m_loop = nullptr;             // 所属事件循环，不拥有生命周期
            SocketType m_fd = kInvalidSocket;        // 被观察的 socket
            IOEvent m_events = IOEvent::None;        // 当前关注事件集合
            IOEvent m_revents = IOEvent::None;       // 本轮就绪事件集合
            Callback m_readCallback;                 // 读事件处理回调
            Callback m_writeCallback;                // 写事件处理回调
            Callback m_closeCallback;                // 关闭事件处理回调
            Callback m_errorCallback;                // 错误事件处理回调
            Index m_index = Index::New;              // Poller 内部状态
        };

        Channel::Channel(EventLoop* loop, SocketType fd, IOEvent events)
            : m_impl(new ChannelImpl{}) {
            m_impl->m_loop = loop;
            m_impl->m_fd = fd;
            m_impl->m_events = events;
        }

        Channel::~Channel() {
            delete m_impl;
            m_impl = nullptr;
        }

        IOEvent Channel::Events() const noexcept {
            return m_impl ? m_impl->m_events : IOEvent::None;
        }

        void Channel::Enable(IOEvent event) {
            if (!m_impl) return;

            const IOEvent oldEvents = m_impl->m_events; // 记录变化前事件集合
            m_impl->m_events |= event;
            UpdateLoopChannel(oldEvents);
        }

        void Channel::Disable(IOEvent event) {
            if (!m_impl) return;

            const IOEvent oldEvents = m_impl->m_events; // 记录变化前事件集合
            m_impl->m_events &= ~event;
            UpdateLoopChannel(oldEvents);
        }

        void Channel::DisableAll() {
            if (!m_impl) return;

            const IOEvent oldEvents = m_impl->m_events; // 记录变化前事件集合
            m_impl->m_events = IOEvent::None;
            UpdateLoopChannel(oldEvents);
        }

        void Channel::EnableReading() {
            Enable(IOEvent::Read);
        }

        void Channel::DisableReading() {
            Disable(IOEvent::Read);
        }

        void Channel::EnableWriting() {
            Enable(IOEvent::Write);
        }

        void Channel::DisableWriting() {
            Disable(IOEvent::Write);
        }

        bool Channel::IsEventEnabled(IOEvent event) const noexcept {
            return HasEvent(Events(), event);
        }

        SocketType Channel::GetSocket() const noexcept {
            return m_impl ? m_impl->m_fd : kInvalidSocket;
        }

        void Channel::SetRevents(IOEvent event) noexcept {
            if (m_impl) m_impl->m_revents = event;
        }

        IOEvent Channel::Revents() const noexcept {
            return m_impl ? m_impl->m_revents : IOEvent::None;
        }

        void Channel::HandleEvent() {
            if (!m_impl) return;

            const IOEvent revents = m_impl->m_revents; // 固定本轮快照，避免回调中修改影响分发
            if (HasEvent(revents, IOEvent::Error) && m_impl->m_errorCallback) m_impl->m_errorCallback();
            if (HasEvent(revents, IOEvent::Close) && m_impl->m_closeCallback) m_impl->m_closeCallback();
            if (HasEvent(revents, IOEvent::Read) && m_impl->m_readCallback) m_impl->m_readCallback();
            if (HasEvent(revents, IOEvent::Write) && m_impl->m_writeCallback) m_impl->m_writeCallback();
        }

        void Channel::SetReadCallback(Callback callback) {
            if (m_impl) m_impl->m_readCallback = std::move(callback);
        }

        void Channel::SetWriteCallback(Callback callback) {
            if (m_impl) m_impl->m_writeCallback = std::move(callback);
        }

        void Channel::SetCloseCallback(Callback callback) {
            if (m_impl) m_impl->m_closeCallback = std::move(callback);
        }

        void Channel::SetErrorCallback(Callback callback) {
            if (m_impl) m_impl->m_errorCallback = std::move(callback);
        }

        Channel::Index Channel::GetIndex() const noexcept {
            return m_impl ? m_impl->m_index : Index::New;
        }

        void Channel::SetIndex(Index index) noexcept {
            if (m_impl) m_impl->m_index = index;
        }

        void Channel::UpdateLoopChannel(IOEvent oldEvents) {
            if (m_impl == nullptr || m_impl->m_loop == nullptr || oldEvents == m_impl->m_events) return;
            (void)m_impl->m_loop->UpdateChannel(this);
        }
    }
}
