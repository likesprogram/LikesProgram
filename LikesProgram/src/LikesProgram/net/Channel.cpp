#include "../../../include/LikesProgram/net/Channel.hpp"
#include "../../../include/LikesProgram/net/Connection.hpp"
#include "../../../include/LikesProgram/net/EventLoop.hpp"

namespace LikesProgram {
    namespace Net {
        Channel::Channel(EventLoop* loop, SocketType fd, IOEvent events, Connection* connection)
            : m_loop(loop), m_fd(fd), m_events(events), m_revents(IOEvent::None), m_connection(connection) {
        }

        IOEvent Channel::Events() const {
            return m_events;
        }

        void Channel::Enable(IOEvent event) {
            IOEvent old = m_events;
            m_events |= event;
            UpdateLoopChannel(old);
        }

        void Channel::Disable(IOEvent event) {
            IOEvent old = m_events;
            m_events &= ~event;
            UpdateLoopChannel(old);
        }

        void Channel::DisableAll() {
            IOEvent old = m_events;
            m_events = IOEvent::None;
            UpdateLoopChannel(old);
        }

        // 快捷封装
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

        bool Channel::IsEventEnabled(IOEvent event) const {
            return (m_events & event) != IOEvent::None;
        }

        SocketType Channel::GetSocket() const {
            return m_fd;
        }

        void Channel::SetRevents(IOEvent event) {
            m_revents = event;
        }

        IOEvent Channel::Revents() const {
            return m_revents;
        }

        Connection* Channel::GetConnection() const {
            return m_connection;
        }

        void Channel::SetConnection(Connection* c) noexcept {
            m_connection = c;
        }

        void Channel::HandleEvent() {
            if (!m_connection) return;

            const IOEvent ev = m_revents;

            // 错误/关闭优先阻止
            if ((ev & IOEvent::Error) != IOEvent::None) {
                m_connection->HandleError();
                return;
            }
            if ((ev & IOEvent::Close) != IOEvent::None) {
                m_connection->HandleClose();
                return;
            }

            // 超时（可与读写并存）
            if ((ev & IOEvent::Timeout) != IOEvent::None) {
                m_connection->HandleTimeout();
            }

            // 读写事件
            if ((ev & IOEvent::Read) != IOEvent::None) {
                m_connection->HandleRead();
            }
            if ((ev & IOEvent::Write) != IOEvent::None) {
                m_connection->HandleWrite();
            }
        }

        Channel::Index Channel::GetIndex() const noexcept {
            return m_index;
        }

        void Channel::SetIndex(Channel::Index idx) noexcept {
            m_index = idx;
        }

        void Channel::UpdateLoopChannel(IOEvent oldEvent) {
            if (oldEvent != m_events && m_loop) m_loop->UpdateChannel(this);
        }
    }
}
