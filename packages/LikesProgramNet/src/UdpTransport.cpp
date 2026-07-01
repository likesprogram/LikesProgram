#include <LikesProgram/Net/UdpTransport.hpp>
#include "net/SocketOps.hpp"
#include <algorithm>
#include <limits>
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct UdpTransport::UdpTransportImpl {
            sockaddr_storage m_lastPeer{};      // 最近一次 recvfrom 的发送方地址
            SocketLength m_lastPeerLength = 0;  // 最近发送方地址长度
            bool m_hasLastPeer = false;         // 是否可向最近发送方回写
        };

        UdpTransport::UdpTransport(SocketType fd)
            : Transport(fd), m_impl(new UdpTransportImpl{}) {
        }

        UdpTransport::~UdpTransport() {
            Close();
            delete m_impl;
            m_impl = nullptr;
        }

        IoResult UdpTransport::ReadSome(Buffer& in) {
            if (SecureCommunicationReady()) {
                // 安全层已接管时才分派到用户实现的安全读钩子。
                return ReadSecureSome(in);
            }

            return ReadSocketSome(in);
        }

        IoResult UdpTransport::WriteSome(const std::uint8_t* data, std::size_t len) {
            if (SecureCommunicationReady()) {
                // 安全层已接管时才分派到用户实现的安全写钩子。
                return WriteSecureSome(data, len);
            }

            return WriteSocketSome(data, len);
        }

        void UdpTransport::ShutdownWrite() {
            if (SecureCommunicationReady()) {
                ShutdownSecureWrite();
                return;
            }

            ShutdownSocketWrite();
        }

        void UdpTransport::Close() {
            CloseSecureLayer();
            CloseSocket();
        }

        IoResult UdpTransport::InitializeSecureLayer() {
            // 默认 UDP transport 是明文数据报；该钩子只初始化连接级安全资源，不改变通信状态。
            return MakeOk(0);
        }

        IoResult UdpTransport::UpgradeCommunication() {
            // 默认不改变 UDP 通信状态，安全升级由派生类显式调用 BeginSecureHandshake 管理。
            return MakeOk(0);
        }

        IoResult UdpTransport::ReadSocketSome(Buffer& in) {
            std::vector<std::uint8_t> temp(65536); // 单个 UDP 数据报读取缓冲，避免占用大栈帧
            sockaddr_storage peer{}; // 本次数据报发送方地址
            SocketLength peerLength = static_cast<SocketLength>(sizeof(peer));

            for (;;) {
                const SocketType fd = CurrentSocket(); // 本轮读取使用的 UDP socket
                const int rc = ::recvfrom(fd,
#ifdef _WIN32
                    reinterpret_cast<char*>(temp.data()),
#else
                    reinterpret_cast<void*>(temp.data()),
#endif
                    static_cast<int>(temp.size()), 0,
                    reinterpret_cast<sockaddr*>(&peer),
                    &peerLength);

                if (rc >= 0) {
                    if (m_impl) {
                        m_impl->m_lastPeer = peer;
                        m_impl->m_lastPeerLength = peerLength;
                        m_impl->m_hasLastPeer = true;
                    }
                    in.Append(temp.data(), static_cast<std::size_t>(rc));
                    return MakeOk(rc);
                }

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        IoResult UdpTransport::WriteSocketSome(const std::uint8_t* data, std::size_t len) {
            if (data == nullptr || len == 0) return MakeOk(0);

            const std::size_t maxChunk =
                static_cast<std::size_t>((std::numeric_limits<int>::max)()); // Winsock/POSIX 参数上限
            const int chunk = static_cast<int>(std::min(len, maxChunk));

            for (;;) {
                int rc = -1; // send/sendto 返回值
                const SocketType fd = CurrentSocket(); // 本轮写入使用的 UDP socket
                if (m_impl && m_impl->m_hasLastPeer) {
                    rc = ::sendto(fd,
#ifdef _WIN32
                        reinterpret_cast<const char*>(data),
#else
                        reinterpret_cast<const void*>(data),
#endif
                        chunk, 0,
                        reinterpret_cast<const sockaddr*>(&m_impl->m_lastPeer),
                        m_impl->m_lastPeerLength);
                }
                else {
                    rc = ::send(fd,
#ifdef _WIN32
                        reinterpret_cast<const char*>(data),
#else
                        reinterpret_cast<const void*>(data),
#endif
                        chunk, 0);
                }

                if (rc >= 0) return MakeOk(rc);

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        void UdpTransport::ShutdownSocketWrite() {
            // UDP 没有连接级半关闭语义，保留空实现给 Connection 统一调用。
        }

        void UdpTransport::CloseSocket() {
            if (!MarkClosedOnce()) return;

            Internal::CloseSocket(CurrentSocket());
            SetCurrentSocket(kInvalidSocket);
            if (m_impl) {
                m_impl->m_hasLastPeer = false;
                m_impl->m_lastPeerLength = 0;
            }
        }

        bool UdpTransport::HasLastPeer() const noexcept {
            return m_impl && m_impl->m_hasLastPeer;
        }

        Address UdpTransport::LastPeerAddress() const {
            if (!m_impl || !m_impl->m_hasLastPeer) return Address();
            return Address(m_impl->m_lastPeer, m_impl->m_lastPeerLength);
        }

        IoResult UdpTransport::ReadSecureSome(Buffer& in) {
            // 未接管 DTLS/自定义安全读时保持普通 UDP socket 能力。
            return ReadSocketSome(in);
        }

        IoResult UdpTransport::WriteSecureSome(const std::uint8_t* data, std::size_t len) {
            // 未接管 DTLS/自定义安全写时保持普通 UDP socket 能力。
            return WriteSocketSome(data, len);
        }

        void UdpTransport::ShutdownSecureWrite() {
            ShutdownSocketWrite();
        }

        void UdpTransport::CloseSecureLayer() {
            // 默认没有安全层资源；用户可释放 DTLS 会话等连接级对象。
        }
    }
}
