#include <LikesProgram/Net/TcpTransport.hpp>
#include "net/SocketOps.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace LikesProgram {
    namespace Net {
        TcpTransport::TcpTransport(SocketType fd)
            : Transport(fd) {
        }

        TcpTransport::~TcpTransport() {
            Close();
        }

        IoResult TcpTransport::ReadSome(Buffer& in) {
            if (SecureCommunicationReady()) {
                // 安全层已接管时才分派到 TLS/SSL 读钩子。
                return ReadSecureSome(in);
            }

            return ReadSocketSome(in);
        }

        IoResult TcpTransport::WriteSome(const std::uint8_t* data, std::size_t len) {
            if (SecureCommunicationReady()) {
                // 安全层已接管时才分派到 TLS/SSL 写钩子。
                return WriteSecureSome(data, len);
            }

            return WriteSocketSome(data, len);
        }

        void TcpTransport::ShutdownWrite() {
            if (SecureCommunicationReady()) {
                ShutdownSecureWrite();
                return;
            }

            ShutdownSocketWrite();
        }

        void TcpTransport::Close() {
            CloseSecureLayer();
            CloseSocket();
        }

        IoResult TcpTransport::InitializeSecureLayer() {
            // 默认 TCP transport 是明文 socket；该钩子只初始化连接级安全资源，不改变通信状态。
            return MakeOk(0);
        }

        IoResult TcpTransport::UpgradeCommunication() {
            // 默认不升级通信层，STARTTLS 等语义由用户派生类调用 BeginSecureHandshake 后定义。
            return MakeOk(0);
        }

        IoResult TcpTransport::ReadSocketSome(Buffer& in) {
            std::array<std::uint8_t, 8192> temp{}; // 单次读取缓冲，避免长期占用大内存
            for (;;) {
                const SocketType fd = CurrentSocket(); // 本轮读取使用的 socket 快照
                const int rc = ::recv(fd,
#ifdef _WIN32
                    reinterpret_cast<char*>(temp.data()),
#else
                    reinterpret_cast<void*>(temp.data()),
#endif
                    static_cast<int>(temp.size()), 0);

                if (rc > 0) {
                    in.Append(temp.data(), static_cast<std::size_t>(rc));
                    return MakeOk(rc);
                }

                if (rc == 0) return MakePeerClosed();

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        IoResult TcpTransport::WriteSocketSome(const std::uint8_t* data, std::size_t len) {
            if (data == nullptr || len == 0) return MakeOk(0);

            const std::size_t maxChunk =
                static_cast<std::size_t>((std::numeric_limits<int>::max)()); // send 接口的安全上限
            const int chunk = static_cast<int>(std::min(len, maxChunk));

            for (;;) {
                const SocketType fd = CurrentSocket(); // 本轮写入使用的 socket 快照
                const int rc = ::send(fd,
#ifdef _WIN32
                    reinterpret_cast<const char*>(data),
#else
                    reinterpret_cast<const void*>(data),
#endif
                    chunk, 0);

                if (rc >= 0) return MakeOk(rc);

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        void TcpTransport::ShutdownSocketWrite() {
            const SocketType fd = CurrentSocket(); // 半关闭不释放所有权
            if (fd == kInvalidSocket) return;
            (void)Internal::ShutdownWrite(fd);
        }

        void TcpTransport::CloseSocket() {
            if (!MarkClosedOnce()) return;

            Internal::CloseSocket(CurrentSocket());
            SetCurrentSocket(kInvalidSocket);
        }

        IoResult TcpTransport::ReadSecureSome(Buffer& in) {
            // 未接管 TLS/SSL 读时保持普通 socket 能力，避免派生类被迫重写所有函数。
            return ReadSocketSome(in);
        }

        IoResult TcpTransport::WriteSecureSome(const std::uint8_t* data, std::size_t len) {
            // 未接管 TLS/SSL 写时保持普通 socket 能力，便于渐进式派生。
            return WriteSocketSome(data, len);
        }

        void TcpTransport::ShutdownSecureWrite() {
            ShutdownSocketWrite();
        }

        void TcpTransport::CloseSecureLayer() {
            // 默认没有安全层资源；用户可释放 SSL* 等连接级对象。
        }
    }
}
