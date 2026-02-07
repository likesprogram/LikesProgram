#include "../../../include/LikesProgram/net/Connection.hpp"
#include "../../../include/LikesProgram/net/Channel.hpp"
#include "../../../include/LikesProgram/net/EventLoop.hpp"

namespace LikesProgram {
	namespace Net {
        Connection::Connection(SocketType fd, EventLoop* loop, std::unique_ptr<Transport> transport)
            : m_fd(fd), m_loop(loop), m_transport(std::move(transport)) {
        }

        Connection::~Connection() {
            // 析构不做复杂逻辑，确保幂等
            if (m_state != State::Closed) {
                DoClose(/*notifyServer*/false);
            }
        }

        void Connection::Start() {

        }

        SocketType Connection::GetSocket() const noexcept {
            return m_fd;
        }

        void Connection::SetChannel(Channel* ch) noexcept {
            m_channel = ch;
        }

        void Connection::Send(const Buffer& buf) {
            Send(buf.Peek(), buf.ReadableBytes());
        }

        void Connection::AdoptChannel(std::unique_ptr<Channel> ch) {
            m_channelOwned = std::move(ch);
            m_channel = m_channelOwned.get();
        }

        void Connection::SetFrameworkCloseCallback(CloseCallback cb) {
            m_onCloseInternal = std::move(cb);
        }

        void Connection::Shutdown() {
            if (m_state == State::Closed) return;

            if (m_loop && !m_loop->IsInLoopThread()) {
                m_loop->PostTask([this] { Shutdown(); });
                return;
            }

            if (m_state == State::Connected) {
                m_state = State::Closing;
                if (m_transport && m_outBuffer.ReadableBytes() == 0) {
                    m_transport->ShutdownWrite();
                }
            }
        }

        void Connection::ForceClose() {
            if (m_state == State::Closed) return;

            if (m_loop && !m_loop->IsInLoopThread()) {
                m_loop->PostTask([this] { ForceClose(); });
                return;
            }

            DoClose(/*notify*/false);
        }

        void Connection::HandleRead() {
            if (m_state == State::Closed || !m_transport) return;

            // TLS 握手阶段：先推进握手（完成前不进应用层）
            if (m_transport->NeedHandshake()) {
                if (!AdvanceHandshake()) return;
            }

            for (;;) {
                const IoResult r = m_transport->ReadSome(m_inBuffer);

                if (r.status == IoStatus::Ok) {
                    if (r.nbytes > 0) {
                        // 业务在 OnMessage 里粘包拆包，并 Consume 已处理字节
                        OnMessage(m_inBuffer);
                        continue; // ET：读到 WouldBlock
                    }
                    // Ok 但 0 字节：通常不出现，退出
                    break;
                }

                if (r.status == IoStatus::WouldBlock) {
                    break;
                }

                if (r.status == IoStatus::PeerClosed) {
                    DoClose(/*notifyServer*/true);
                    return;
                }

                // Error
                OnError(r.err);
                DoClose(/*notifyServer*/true);
                return;
            }
        }

        void Connection::HandleTimeout() {
            if (m_state == State::Closed) return;
            OnTimeout();
        }

        void Connection::HandleWrite() {
            if (m_state == State::Closed || !m_transport) return;

            // TLS 握手阶段：先推进握手
            if (m_transport->NeedHandshake()) {
                if (!AdvanceHandshake()) return;
            }

            bool madeProgress = false;

            while (m_outBuffer.ReadableBytes() > 0) {
                const IoResult r = m_transport->WriteSome(m_outBuffer);

                if (r.status == IoStatus::Ok) {
                    if (r.nbytes > 0) {
                        m_outBuffer.Consume((size_t)r.nbytes);
                        madeProgress = true;
                        continue;
                    }
                    // Ok 但 0：视为无进展，避免死循环
                    break;
                }

                if (r.status == IoStatus::WouldBlock) {
                    // 仍需关注写事件
                    EnableWritingIfNeeded();
                    return;
                }

                // Error / PeerClosed（理论上写不应 PeerClosed，但统一处理）
                OnError(r.err);
                DoClose(/*notifyServer*/true);
                return;
            }

            // outBuffer 已空：关写事件，通知业务“写完了”
            if (m_outBuffer.ReadableBytes() == 0) {
                DisableWriting();
                if (madeProgress) OnWriteComplete();
            }

            // Closing 且已写完：shutdownWrite（发送 FIN / TLS close_notify 由 Transport 决定）
            if (m_state == State::Closing && m_outBuffer.ReadableBytes() == 0) {
                m_transport->ShutdownWrite();
            }
        }

        void Connection::HandleClose() {
            DoClose(/*notify*/true);
        }

        void Connection::HandleError() {
            DoClose(/*notify*/true);
        }

        void Connection::SetCloseCallbackInternal(CloseCallback cb) {
            m_onCloseInternal = std::move(cb);
        }

        void Connection::SendInLoop(const void* data, size_t len) {
            if (!m_transport || m_state == State::Closed) return;

            // outBuffer 为空：尝试直接写，减少延迟
            if (m_outBuffer.ReadableBytes() == 0) {
                Buffer tmp;
                tmp.Append(data, len);

                const IoResult r = m_transport->WriteSome(tmp);

                if (r.status == IoStatus::Ok) {
                    if (r.nbytes > 0) tmp.Consume((size_t)r.nbytes);

                    // 仍有剩余：进入 outBuffer，打开写事件
                    if (tmp.ReadableBytes() > 0) {
                        m_outBuffer.Append(tmp);
                        EnableWritingIfNeeded();
                    }
                    else {
                        // 全部直接写完
                        OnWriteComplete();
                    }
                    return;
                }

                if (r.status == IoStatus::WouldBlock) {
                    // 全部进 outBuffer
                    m_outBuffer.Append(data, len);
                    EnableWritingIfNeeded();
                    return;
                }

                // Error / PeerClosed
                OnError(r.err);
                DoClose(/*notifyServer*/true);
                return;
            }

            // 有积压：追加并确保监听写
            m_outBuffer.Append(data, len);
            EnableWritingIfNeeded();
        }

        bool Connection::AdvanceHandshake() {
            const IoResult r = m_transport->Handshake();

            if (r.status == IoStatus::Ok) {
                // 握手完成：恢复读关注；写关注按 outBuffer
                EnableReading();
                EnableWritingIfNeeded();
                OnConnected();
                return true;
            }

            if (r.status == IoStatus::WouldBlock) {
                // WANT_READ/WANT_WRITE：动态调整关注事件（Channel 需支持 DisableReading/DisableWriting）
                if (m_transport->RemainWantRead()) EnableReading(); else DisableReading();
                if (m_transport->RemainWantWrite()) EnableWriting(); else DisableWriting();
                return false;
            }

            // Error
            OnError(r.err);
            DoClose(/*notifyServer*/true);
            return false;
        }

        void Connection::DoClose(bool notifyServer) {
            if (m_state == State::Closed) return;

            OnClosing();

            m_state = State::Closed;

            // 先停事件，避免 close 后仍触发
            if (m_channel) {
                m_channel->DisableAll();
                if (m_loop) m_loop->UnregisterChannel(m_channel);
            }

            if (m_transport) {
                m_transport->Close();
            }

            // 通知 Server 清理连接表（框架职责）
            if (notifyServer && m_onCloseInternal) {
                m_onCloseInternal(*this);
            }

            // 最后通知业务
            OnClosed();
        }

        void Connection::EnableReading() { if (m_channel) m_channel->EnableReading(); }
        void Connection::DisableReading() { if (m_channel) m_channel->DisableReading(); }

        void Connection::EnableWriting() { if (m_channel) m_channel->EnableWriting(); }
        void Connection::DisableWriting() { if (m_channel) m_channel->DisableWriting(); }

        void Connection::EnableWritingIfNeeded() {
            if (m_channel && m_outBuffer.ReadableBytes() > 0) {
                m_channel->EnableWriting();
                if (m_loop) m_loop->UpdateChannel(m_channel);
            }
        }

        void Connection::Send(const void* data, size_t len) {
            if (m_state == State::Closed) return;
            if (!data || len == 0) return;

            Buffer copy;
            copy.Append(data, len);

            if (m_loop && !m_loop->IsInLoopThread()) {
                // 捕获 shared_ptr，避免 this 悬空
                auto self = shared_from_this();
                m_loop->PostTask([self, buf = std::move(copy)]() mutable {
                    if (self->m_state == State::Closed) return;
                    self->SendInLoop(buf.Peek(), buf.ReadableBytes());
                    });
                return;
            }

            SendInLoop(copy.Peek(), copy.ReadableBytes());
        }
	}
}