#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/TcpTransport.hpp>
#include <algorithm>
#include <atomic>
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct Connection::ConnectionImpl {
            SocketType m_fd = kInvalidSocket;                  // 当前连接 socket
            EventLoop* m_loop = nullptr;                       // 所属事件循环，不拥有生命周期
            std::unique_ptr<Transport> m_transport;            // TCP/UDP 或用户派生安全传输对象
            std::unique_ptr<Channel> m_channel;                // 连接专属 Channel
            bool m_startingCallbacks = false;                  // Start 回调期允许同步推进握手切换
            Address m_remoteAddress;                           // 对端地址缓存
            Address m_localAddress;                            // 本端地址缓存
            Buffer m_inBuffer;                                 // 输入缓冲区
            Buffer m_outBuffer;                                // 输出缓冲区
            CloseCallback m_closeCallback;                     // 框架关闭回调
            std::atomic<State> m_state{ State::Connected };    // 连接状态机

            ConnectionImpl()
                : m_state(State::Connected) {
            }
        };

        namespace {
            const Address& EmptyAddress() {
                static const Address empty; // moved-from/closed 连接的稳定空地址引用
                return empty;
            }
        }

        Connection::Connection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport)
            : m_impl(new ConnectionImpl{}) {
            m_impl->m_fd = fd;
            m_impl->m_loop = loop;
            m_impl->m_transport = std::move(transport);
            m_impl->m_remoteAddress = Address::GetRemoteAddress(fd);
            m_impl->m_localAddress = Address::GetLocalAddress(fd);
            if (!m_impl->m_transport) m_impl->m_transport = std::make_unique<TcpTransport>(fd);
        }

        Connection::~Connection() {
            DoClose(false);
            delete m_impl;
            m_impl = nullptr;
        }

        void Connection::Start() {
            if (m_impl == nullptr || m_impl->m_loop == nullptr || m_impl->m_fd == kInvalidSocket) return;

            if (m_impl->m_transport) {
                const IoResult initResult = m_impl->m_transport->InitializeSecureLayer(); // 允许用户派生类创建 TLS/SSL 会话
                if (initResult.status == IoStatus::Error) {
                    OnError(initResult.error);
                    DoClose(true);
                    return;
                }
                if (initResult.status == IoStatus::PeerClosed) {
                    DoClose(true);
                    return;
                }
            }

            m_impl->m_channel = std::make_unique<Channel>(m_impl->m_loop, m_impl->m_fd, IOEvent::None);
            m_impl->m_channel->SetReadCallback([this]() { HandleRead(); });
            m_impl->m_channel->SetWriteCallback([this]() { HandleWrite(); });
            m_impl->m_channel->SetCloseCallback([this]() { HandleClose(); });
            m_impl->m_channel->SetErrorCallback([this]() { HandleError(); });

            (void)m_impl->m_loop->RegisterChannel(m_impl->m_channel.get());

            m_impl->m_startingCallbacks = true;
            OnSecureLayerReady();
            if (GetState() == State::Closed) {
                m_impl->m_startingCallbacks = false;
                return;
            }
            RefreshTransportInterest();
            OnConnected();
            m_impl->m_startingCallbacks = false;
        }

        SocketType Connection::GetSocket() const noexcept {
            return m_impl ? m_impl->m_fd : kInvalidSocket;
        }

        Connection::State Connection::GetState() const noexcept {
            return m_impl ? m_impl->m_state.load(std::memory_order_acquire) : State::Closed;
        }

        bool Connection::IsConnected() const noexcept {
            return GetState() == State::Connected;
        }

        void Connection::SetFrameworkCloseCallback(CloseCallback callback) {
            if (m_impl) m_impl->m_closeCallback = std::move(callback);
        }

        void Connection::Send(const Buffer& buffer) {
            Send(buffer.Peek(), buffer.ReadableBytes());
        }

        void Connection::Send(const void* data, std::size_t len) {
            if (data == nullptr || len == 0 || GetState() == State::Closed) return;

            const auto* first = static_cast<const std::uint8_t*>(data);
            std::vector<std::uint8_t> payload(first, first + len); // 跨线程投递必须复制用户缓冲
            QueueInLoop([this, payload = std::move(payload)]() {
                SendInLoop(payload.data(), payload.size());
            });
        }

        void Connection::RunInLoop(Task task) {
            if (!task) return;
            if (m_impl != nullptr && m_impl->m_loop != nullptr && m_impl->m_loop->IsInLoopThread()) {
                task();
            }
            else if (m_impl != nullptr && m_impl->m_startingCallbacks) {
                task();
            }
            else {
                QueueInLoop(std::move(task));
            }
        }

        void Connection::QueueInLoop(Task task) {
            if (!task) return;
            if (m_impl == nullptr || m_impl->m_loop == nullptr) return;
            m_impl->m_loop->PostTask(std::move(task));
        }

        void Connection::Shutdown() {
            QueueInLoop([this]() {
                if (m_impl == nullptr) return;
                if (m_impl->m_state.load(std::memory_order_acquire) == State::Connected) {
                    m_impl->m_state.store(State::Closing, std::memory_order_release);
                    if (m_impl->m_outBuffer.ReadableBytes() == 0 && m_impl->m_transport) {
                        m_impl->m_transport->ShutdownWrite();
                    }
                }
            });
        }

        void Connection::ForceClose() {
            QueueInLoop([this]() { DoClose(true); });
        }

        void Connection::UpgradeCommunication() {
            RunInLoop([this]() {
                if (m_impl == nullptr || !m_impl->m_transport || GetState() == State::Closed) return;

                const IoResult result = m_impl->m_transport->UpgradeCommunication(); // 由派生 transport 决定是否进入握手态
                if (result.status == IoStatus::Ok || result.status == IoStatus::WouldBlock) {
                    RefreshTransportInterest();
                    EnableWritingIfNeeded();
                    return;
                }

                if (result.status == IoStatus::PeerClosed) {
                    DoClose(true);
                    return;
                }

                OnError(result.error);
                DoClose(true);
            });
        }

        void Connection::HandleRead() {
            if (m_impl == nullptr || !m_impl->m_transport || GetState() == State::Closed) return;

            if (m_impl->m_transport->NeedHandshake()) {
                (void)AdvanceHandshake();
                return;
            }

            const IoResult result = m_impl->m_transport->ReadSome(m_impl->m_inBuffer);
            if (result.status == IoStatus::Ok) {
                OnMessage(m_impl->m_inBuffer);
            }
            else if (result.status == IoStatus::PeerClosed) {
                HandleClose();
            }
            else if (result.status == IoStatus::Error) {
                OnError(result.error);
                HandleClose();
            }
        }

        void Connection::HandleWrite() {
            if (m_impl == nullptr || !m_impl->m_transport || !m_impl->m_channel || GetState() == State::Closed) return;

            if (m_impl->m_transport->NeedHandshake()) {
                if (!AdvanceHandshake() || m_impl->m_transport->NeedHandshake()) return;
            }

            while (m_impl->m_outBuffer.ReadableBytes() > 0) {
                const IoResult result = m_impl->m_transport->WriteSome(
                    m_impl->m_outBuffer.Peek(),
                    m_impl->m_outBuffer.ReadableBytes());
                if (result.status == IoStatus::Ok) {
                    m_impl->m_outBuffer.Consume(static_cast<std::size_t>(std::max<std::int64_t>(result.nbytes, 0)));
                    if (result.nbytes == 0) break;
                }
                else if (result.status == IoStatus::WouldBlock) {
                    break;
                }
                else {
                    OnError(result.error);
                    DoClose(true);
                    return;
                }
            }

            if (m_impl->m_outBuffer.ReadableBytes() == 0) {
                m_impl->m_channel->DisableWriting();
                OnWriteComplete();
                if (GetState() == State::Closing && m_impl->m_transport) m_impl->m_transport->ShutdownWrite();
            }
        }

        void Connection::HandleClose() {
            DoClose(true);
        }

        void Connection::HandleError() {
            OnError(0);
        }

        const Address& Connection::GetRemoteAddress() const noexcept {
            return m_impl ? m_impl->m_remoteAddress : EmptyAddress();
        }

        const Address& Connection::GetLocalAddress() const noexcept {
            return m_impl ? m_impl->m_localAddress : EmptyAddress();
        }

        void Connection::SendInLoop(const std::uint8_t* data, std::size_t len) {
            if (m_impl == nullptr || data == nullptr || len == 0 || GetState() == State::Closed) return;

            if (m_impl->m_transport && m_impl->m_transport->NeedHandshake()) {
                m_impl->m_outBuffer.Append(data, len);
                RefreshTransportInterest();
                return;
            }

            if (m_impl->m_outBuffer.ReadableBytes() == 0 && m_impl->m_transport) {
                const IoResult result = m_impl->m_transport->WriteSome(data, len);
                if (result.status == IoStatus::Ok) {
                    const std::size_t written = static_cast<std::size_t>(std::max<std::int64_t>(result.nbytes, 0));
                    if (written >= len) {
                        OnWriteComplete();
                        return;
                    }

                    m_impl->m_outBuffer.Append(data + written, len - written);
                }
                else if (result.status == IoStatus::WouldBlock) {
                    m_impl->m_outBuffer.Append(data, len);
                }
                else {
                    OnError(result.error);
                    DoClose(true);
                    return;
                }
            }
            else {
                m_impl->m_outBuffer.Append(data, len);
            }

            EnableWritingIfNeeded();
        }

        bool Connection::AdvanceHandshake() {
            if (m_impl == nullptr || !m_impl->m_transport || !m_impl->m_transport->NeedHandshake()) return true;

            const IoResult result = m_impl->m_transport->Handshake();
            if (result.status == IoStatus::Ok) {
                if (!m_impl->m_transport->NeedHandshake()) {
                    OnHandshakeDone();
                    RefreshTransportInterest();
                    EnableWritingIfNeeded();
                }
                else {
                    RefreshTransportInterest();
                }
                return true;
            }

            if (result.status == IoStatus::WouldBlock) {
                RefreshTransportInterest();
                return true;
            }

            if (result.status == IoStatus::PeerClosed) {
                DoClose(true);
                return false;
            }

            OnError(result.error);
            DoClose(true);
            return false;
        }

        void Connection::RefreshTransportInterest() {
            if (m_impl == nullptr || !m_impl->m_channel) return;

            if (m_impl->m_transport && m_impl->m_transport->NeedHandshake()) {
                if (m_impl->m_transport->RemainWantRead()) {
                    m_impl->m_channel->EnableReading();
                }
                else {
                    m_impl->m_channel->DisableReading();
                }

                if (m_impl->m_transport->RemainWantWrite()) {
                    m_impl->m_channel->EnableWriting();
                }
                else if (m_impl->m_outBuffer.ReadableBytes() == 0) {
                    m_impl->m_channel->DisableWriting();
                }
                return;
            }

            m_impl->m_channel->EnableReading();
            if (m_impl->m_outBuffer.ReadableBytes() == 0) m_impl->m_channel->DisableWriting();
        }

        void Connection::EnableWritingIfNeeded() {
            if (m_impl != nullptr && m_impl->m_channel != nullptr && m_impl->m_outBuffer.ReadableBytes() > 0) {
                m_impl->m_channel->EnableWriting();
            }
        }

        void Connection::DoClose(bool notifyFramework) {
            if (m_impl == nullptr) return;

            State expected = State::Connected; // 首先尝试从连接态关闭
            if (!m_impl->m_state.compare_exchange_strong(expected, State::Closed, std::memory_order_acq_rel)) {
                expected = State::Closing;
                if (!m_impl->m_state.compare_exchange_strong(expected, State::Closed, std::memory_order_acq_rel)) return;
            }

            OnClosing();

            if (notifyFramework && m_impl->m_channel && m_impl->m_loop) {
                m_impl->m_channel->DisableAll();
                (void)m_impl->m_loop->UnregisterChannel(m_impl->m_channel.get());
            }

            if (m_impl->m_transport) m_impl->m_transport->Close();

            std::shared_ptr<Connection> self; // 从 EventLoop 移除后延长当前关闭流程生命周期
            if (notifyFramework && m_impl->m_loop != nullptr && m_impl->m_fd != kInvalidSocket) {
                self = m_impl->m_loop->DetachConnection(m_impl->m_fd);
            }

            if (notifyFramework && m_impl->m_closeCallback) m_impl->m_closeCallback(*this);
            OnClosed();
            m_impl->m_fd = kInvalidSocket;
        }

    }
}
