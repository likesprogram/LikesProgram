#include "net/UdpTransport.hpp"
#include "net/platform/SocketOps.hpp"
#include "net/platform/UdpBatchOps.hpp"
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct UdpTransport::UdpTransportImpl {
            sockaddr_storage m_lastPeer{};      // 最近一次 recvfrom 的发送方地址
            SocketLength m_lastPeerLength = 0;  // 最近发送方地址长度
            bool m_hasLastPeer = false;         // 是否可向最近发送方回写
        };

        namespace {
            int InvalidUdpPeerError() noexcept {
                return Internal::InvalidSocketArgumentError();
            }
        }

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

        IoResult UdpTransport::SendTo(const Address& peer, const std::uint8_t* data, std::size_t len) {
            if (SecureCommunicationReady()) {
                // 安全层可选择按 peer 处理 DTLS/自定义 datagram 发送。
                return SendSecureTo(peer, data, len);
            }

            return SendSocketTo(peer, data, len);
        }

        IoResult UdpTransport::ReadBatch(UdpReceiveDatagram* datagrams, std::size_t count) {
            if (SecureCommunicationReady()) {
                // 安全层可一次解包多条 datagram；默认实现仍保持明文批量语义。
                return ReadSecureBatch(datagrams, count);
            }

            return ReadSocketBatch(datagrams, count);
        }

        IoResult UdpTransport::SendBatch(UdpSendDatagram* datagrams, std::size_t count) {
            if (SecureCommunicationReady()) {
                // 安全层可按 peer 批量封包；默认实现仍保持明文批量语义。
                return SendSecureBatch(datagrams, count);
            }

            return SendSocketBatch(datagrams, count);
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
                const std::int64_t rc = Internal::ReceiveSocketFrom(
                    fd,
                    temp.data(),
                    temp.size(),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    &peerLength); // 平台层统一 recvfrom 缓冲与长度类型

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

            for (;;) {
                std::int64_t rc = -1; // send/sendto 返回值
                const SocketType fd = CurrentSocket(); // 本轮写入使用的 UDP socket
                if (m_impl && m_impl->m_hasLastPeer) {
                    Address peer(m_impl->m_lastPeer, m_impl->m_lastPeerLength);
                    return SendSocketTo(peer, data, len);
                }
                else {
                    rc = Internal::SendSocket(fd, data, len, 0); // connected UDP 复用平台发送入口
                }

                if (rc >= 0) return MakeOk(rc);

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        IoResult UdpTransport::SendSocketTo(
            const Address& peer,
            const std::uint8_t* data,
            std::size_t len) {
            if (data == nullptr || len == 0) return MakeOk(0);
            if (!peer.IsValid() || peer.SockAddr() == nullptr || peer.Length() == 0) {
                return MakeError(InvalidUdpPeerError());
            }

            for (;;) {
                const SocketType fd = CurrentSocket(); // 本轮发送使用的 UDP socket
                const std::int64_t rc = Internal::SendSocketTo(
                    fd,
                    data,
                    len,
                    0,
                    peer.SockAddr(),
                    peer.Length()); // 平台层统一 sendto 缓冲与长度类型
                if (rc >= 0) return MakeOk(rc);

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeWouldBlock();
                return MakeError(error);
            }
        }

        IoResult UdpTransport::ReadSocketBatch(UdpReceiveDatagram* datagrams, std::size_t count) {
            auto readFallback = [this, datagrams, count]() -> IoResult {
                if (datagrams == nullptr || count == 0) return MakeOk(0);

                std::int64_t totalBytes = 0; // 已成功读取的总字节数，用于聚合返回。
                std::size_t completed = 0;   // 已完成的 datagram 数，用于区分首条失败和部分成功。
                for (std::size_t i = 0; i < count; ++i) {
                    if (datagrams[i].buffer == nullptr) {
                        datagrams[i].result = MakeError(InvalidUdpPeerError());
                        return completed > 0 ? MakeOk(totalBytes) : datagrams[i].result;
                    }

                    datagrams[i].result = ReadSocketSome(*datagrams[i].buffer);
                    if (datagrams[i].result.status != IoStatus::Ok) {
                        return completed > 0 ? MakeOk(totalBytes) : datagrams[i].result;
                    }

                    totalBytes += datagrams[i].result.nbytes;
                    ++completed;
                    if (datagrams[i].peer != nullptr) {
                        *datagrams[i].peer = LastPeerAddress();
                    }
                }

                return MakeOk(totalBytes);
            };

            if (datagrams == nullptr || count == 0) return MakeOk(0);

            sockaddr_storage lastPeer{}; // 原生批处理最后完成的数据报 peer
            SocketLength lastPeerLength = 0; // 原生批处理最后 peer 的系统结构长度
            IoResult nativeResult{}; // 平台原生批处理聚合结果
            if (Internal::TryReceiveUdpBatch(
                CurrentSocket(),
                datagrams,
                count,
                &lastPeer,
                &lastPeerLength,
                &nativeResult)) {
                if (m_impl && nativeResult.status == IoStatus::Ok && lastPeerLength > 0) {
                    // 通用层只维护最近 peer 语义，批处理系统结构由平台层填充。
                    m_impl->m_lastPeer = lastPeer;
                    m_impl->m_lastPeerLength = lastPeerLength;
                    m_impl->m_hasLastPeer = true;
                }
                return nativeResult;
            }

            return readFallback();
        }

        IoResult UdpTransport::SendSocketBatch(UdpSendDatagram* datagrams, std::size_t count) {
            auto sendFallback = [this, datagrams, count]() -> IoResult {
                if (datagrams == nullptr || count == 0) return MakeOk(0);

                std::int64_t totalBytes = 0; // 已成功发送的总字节数，用于聚合返回。
                std::size_t completed = 0;   // 已完成的 datagram 数，用于区分首条失败和部分成功。
                for (std::size_t i = 0; i < count; ++i) {
                    if (datagrams[i].peer == nullptr) {
                        datagrams[i].result = MakeError(InvalidUdpPeerError());
                        return completed > 0 ? MakeOk(totalBytes) : datagrams[i].result;
                    }

                    datagrams[i].result = SendSocketTo(*datagrams[i].peer, datagrams[i].data, datagrams[i].len);
                    if (datagrams[i].result.status != IoStatus::Ok) {
                        return completed > 0 ? MakeOk(totalBytes) : datagrams[i].result;
                    }

                    totalBytes += datagrams[i].result.nbytes;
                    ++completed;
                }

                return MakeOk(totalBytes);
            };

            if (datagrams == nullptr || count == 0) return MakeOk(0);

            IoResult nativeResult{}; // 平台原生批量发送聚合结果
            if (Internal::TrySendUdpBatch(
                CurrentSocket(),
                datagrams,
                count,
                &nativeResult)) {
                return nativeResult;
            }

            return sendFallback();
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

        IoResult UdpTransport::SendSecureTo(
            const Address& peer,
            const std::uint8_t* data,
            std::size_t len) {
            // 未接管 DTLS/自定义安全写时保持普通 UDP sendto 能力。
            return SendSocketTo(peer, data, len);
        }

        IoResult UdpTransport::ReadSecureBatch(UdpReceiveDatagram* datagrams, std::size_t count) {
            // 未接管 DTLS/自定义安全批量读时保持普通 UDP 批量能力。
            return ReadSocketBatch(datagrams, count);
        }

        IoResult UdpTransport::SendSecureBatch(UdpSendDatagram* datagrams, std::size_t count) {
            // 未接管 DTLS/自定义安全批量写时保持普通 UDP 批量能力。
            return SendSocketBatch(datagrams, count);
        }

        void UdpTransport::ShutdownSecureWrite() {
            ShutdownSocketWrite();
        }

        void UdpTransport::CloseSecureLayer() {
            // 默认没有安全层资源；用户可释放 DTLS 会话等连接级对象。
        }
    }
}
