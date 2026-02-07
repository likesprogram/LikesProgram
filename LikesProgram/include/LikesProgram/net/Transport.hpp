#pragma once
#include "Buffer.hpp"
#include "SocketType.hpp"
#include <atomic>

namespace LikesProgram {
    namespace Net {
        enum class IoStatus {
            Ok,          // 有进展（n > 0，或 n==0 对 write 也可能表示没写入但成功）
            WouldBlock,  // EAGAIN/EWOULDBLOCK 或 SSL_WANT_READ/WRITE
            PeerClosed,  // read==0 或 TLS close_notify
            Error        // 其他错误
        };

        struct IoResult {
            IoStatus status;
            int64_t nbytes;   // Ok 时有效；PeerClosed 通常为 0
            int err;          // Error 时保存 errno / WSAGetLastError / openssl err
        };

        // 传输层接口（抽象基类）
        // 封装底层 TCP/TLS 的读写操作
        class Transport {
        public:
            explicit Transport(SocketType fd) : m_fd(fd) {}
            virtual ~Transport() = default;

            // 从 socket/ssl 读到 inBuffer（append），返回结果
            virtual IoResult ReadSome(Buffer& in) = 0;

            // 从 outBuffer 里尽可能写到 socket/ssl（按 Buffer 可读区域写），返回结果
            virtual IoResult WriteSome(const Buffer& out) = 0;

            // 半关闭（优雅关闭写端）与全关闭分离
            virtual void ShutdownWrite() = 0;
            virtual void Close() = 0;

            // 是否需要握手（TLS）
            virtual bool NeedHandshake() const { return false; }

            // 握手推进：可能需要读/写事件继续推进
            virtual IoResult Handshake() { return { IoStatus::Ok, 0, 0 }; }

            // 握手阶段希望关注的事件（TLS 需要）
            virtual bool RemainWantRead() const { return true; }
            virtual bool RemainWantWrite() const { return false; }
        protected:
            SocketType m_fd{ (SocketType)-1 };
            std::atomic<bool> m_closed{ false };
        };

        // TCP 明文传输实现
        class TcpTransport : public Transport {
        public:
            explicit TcpTransport(SocketType fd) : Transport(fd) {}
            ~TcpTransport() override { Close(); }

            IoResult ReadSome(Buffer& in) override;
            IoResult WriteSome(const Buffer& out) override;

            void ShutdownWrite() override;
            void Close() override;
        };
    }
}
