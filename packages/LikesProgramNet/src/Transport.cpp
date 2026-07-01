#include <LikesProgram/Net/Transport.hpp>
#include <atomic>

namespace LikesProgram {
    namespace Net {
        struct Transport::TransportImpl {
            SocketType m_fd = kInvalidSocket;          // 当前拥有的 socket
            std::atomic<bool> m_closed{ false };       // 防止重复关闭
            std::atomic<TransportSecurityState> m_securityState{
                TransportSecurityState::Plain
            };                                         // 安全层状态机
        };

        Transport::Transport(SocketType fd)
            : m_impl(new TransportImpl{}) {
            m_impl->m_fd = fd;
        }

        Transport::~Transport() {
            delete m_impl;
            m_impl = nullptr;
        }

        SocketType Transport::Fd() const noexcept {
            return CurrentSocket();
        }

        SocketType Transport::DetachFd() noexcept {
            const SocketType fd = CurrentSocket(); // 返回前保留旧 socket
            SetCurrentSocket(kInvalidSocket);
            MarkClosed();
            return fd;
        }

        TransportSecurityState Transport::SecurityState() const noexcept {
            return m_impl
                ? m_impl->m_securityState.load(std::memory_order_acquire)
                : TransportSecurityState::Plain;
        }

        bool Transport::SecureCommunicationReady() const noexcept {
            return SecurityState() == TransportSecurityState::Secure;
        }

        IoResult Transport::InitializeSecureLayer() {
            // 明文 transport 没有额外安全层，派生类可在这里创建 TLS/SSL 会话。
            return MakeOk(0);
        }

        IoResult Transport::UpgradeCommunication() {
            // 默认不改变通信状态，STARTTLS 或自定义安全层由用户派生类自行推进。
            return MakeOk(0);
        }

        bool Transport::NeedHandshake() const {
            return SecurityState() == TransportSecurityState::Handshaking;
        }

        IoResult Transport::Handshake() {
            if (NeedHandshake()) {
                // 默认握手只完成状态切换，真实 TLS/SSL 由派生类覆盖。
                CompleteSecureHandshake();
            }
            return MakeOk(0);
        }

        bool Transport::RemainWantRead() const {
            return true;
        }

        bool Transport::RemainWantWrite() const {
            return false;
        }

        void Transport::BeginSecureHandshake() noexcept {
            if (m_impl) m_impl->m_securityState.store(TransportSecurityState::Handshaking, std::memory_order_release);
        }

        void Transport::CompleteSecureHandshake() noexcept {
            if (m_impl) m_impl->m_securityState.store(TransportSecurityState::Secure, std::memory_order_release);
        }

        void Transport::ResetToPlainCommunication() noexcept {
            if (m_impl) m_impl->m_securityState.store(TransportSecurityState::Plain, std::memory_order_release);
        }

        bool Transport::SecureLayerActive() const noexcept {
            return SecurityState() != TransportSecurityState::Plain;
        }

        SocketType Transport::CurrentSocket() const noexcept {
            return m_impl ? m_impl->m_fd : kInvalidSocket;
        }

        void Transport::SetCurrentSocket(SocketType fd) noexcept {
            if (m_impl) m_impl->m_fd = fd;
        }

        bool Transport::MarkClosedOnce() noexcept {
            if (m_impl == nullptr) return false;

            bool expected = false; // CAS 只允许第一个关闭者释放系统 socket
            return m_impl->m_closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
        }

        void Transport::MarkClosed() noexcept {
            if (m_impl) m_impl->m_closed.store(true, std::memory_order_release);
        }

        IoResult Transport::MakeOk(std::int64_t nbytes) {
            return IoResult{ IoStatus::Ok, nbytes, 0 };
        }

        IoResult Transport::MakeWouldBlock() {
            return IoResult{ IoStatus::WouldBlock, 0, 0 };
        }

        IoResult Transport::MakePeerClosed() {
            return IoResult{ IoStatus::PeerClosed, 0, 0 };
        }

        IoResult Transport::MakeError(int error) {
            return IoResult{ IoStatus::Error, 0, error };
        }
    }
}
