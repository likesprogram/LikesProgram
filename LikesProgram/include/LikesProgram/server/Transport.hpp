#pragma once
#include "Buffer.hpp"
#include <atomic>

namespace LikesProgram {
    namespace Server {

        // 传输层接口（抽象基类）
        // 封装底层 TCP/TLS 的读写操作
        class Transport {
        public:
            virtual ~Transport() = default;

            // 读取数据到 Buffer，返回读取字节数
            virtual int Read(Buffer& buf) = 0;

            // 写入数据 Buffer，返回写入字节数
            virtual int Write(const Buffer& buf) = 0;

            // 主动关闭连接
            virtual void Close() = 0;

            // 可选：TLS 握手
            virtual void SSLHandshake() {}
        };

        // TCP 明文传输实现
        class TcpTransport : public Transport {
        public:
            TcpTransport(int fd) : m_fd(fd), m_closed(false) {}

            ~TcpTransport() override {
                Close(); // 析构时关闭
            }

            // 读取数据
            int Read(Buffer& buf) override {
                if (m_closed) return -1;
                // 使用 recv 或 read 调用
                // 示例：buf.append(...)
                return 0; // 返回读取字节数
            }

            // 写入数据
            int Write(const Buffer& buf) override {
                if (m_closed) return -1;
                // 使用 send 或 write 调用
                return 0; // 返回写入字节数
            }

            // 关闭连接
            void Close() override {
                if (m_closed.exchange(true)) return;
                // 调用 close(fd)
            }

        private:
            int m_fd;                   // TCP socket 文件描述符
            std::atomic<bool> m_closed; // 是否已关闭
        };

        // TLS/SSL 传输实现
        class TlsTransport : public Transport {
        public:
            TlsTransport(int fd) : m_fd(fd), m_closed(false), m_tlsReady(false) {}

            ~TlsTransport() override {
                Close(); // 析构时关闭
            }

            int Read(Buffer& buf) override {
                if (m_closed || !m_tlsReady) return -1;
                // 调用 SSL_read
                return 0; // 返回解密后的字节数
            }

            int Write(const Buffer& buf) override {
                if (m_closed || !m_tlsReady) return -1;
                // 调用 SSL_write
                return 0; // 返回加密写入字节数
            }

            void Close() override {
                if (m_closed.exchange(true)) return;
                // SSL_shutdown + close(fd)
            }

            // TLS 握手
            void SSLHandshake() override {
                if (m_closed) return;
                // SSL_accept / SSL_connect
                m_tlsReady = true;
            }

        private:
            int m_fd;                   // TCP socket fd
            std::atomic<bool> m_closed; // 是否已关闭
            bool m_tlsReady;             // TLS 是否就绪
        };

    } // namespace Server
} // namespace LikesProgram
