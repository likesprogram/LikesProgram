#include "net/TcpTransport.hpp"
#include "net/platform/SocketOps.hpp"
#include <array>

namespace LikesProgram {
    namespace Net {
        TcpTransport::TcpTransport(SocketType fd)
            : TcpTransport(fd, TcpUpgradeMode::Manual) {
        }

        TcpTransport::TcpTransport(SocketType fd, TcpUpgradeMode upgradeMode)
            : Transport(fd),
            m_upgradeMode(upgradeMode) {
        }

        TcpTransport::~TcpTransport() {
            Close();
        }

        IoResult TcpTransport::ReadSome(Buffer& in) {
            if (m_upgradeMode == TcpUpgradeMode::Auto
                && !m_autoPlainDataSeen
                && !SecureLayerActive()) {
                // Auto 服务端先偷看首包：像 TLS 则切入握手，否则继续明文 STARTTLS 路径。
                const IoResult autoResult = TryAutoUpgradeFromSocket();
                if (autoResult.status != IoStatus::Ok) return autoResult;
                if (NeedHandshake() || SecureLayerActive()) return MakeWouldBlock();
            }

            if (m_upgradeMode != TcpUpgradeMode::Disabled && SecureCommunicationReady()) {
                // 安全层已接管时才分派到 TLS/SSL 读钩子。
                return ReadSecureSome(in);
            }

            IoResult result = ReadSocketSome(in);
            if (m_upgradeMode == TcpUpgradeMode::Auto
                && result.status == IoStatus::Ok
                && result.nbytes > 0
                && !SecureLayerActive()) {
                // 已经把首批明文交给协议层后，Auto 不再误判后续 STARTTLS 命令文本。
                m_autoPlainDataSeen = true;
            }
            return result;
        }

        IoResult TcpTransport::WriteSome(const std::uint8_t* data, std::size_t len) {
            if (m_upgradeMode != TcpUpgradeMode::Disabled && SecureCommunicationReady()) {
                // 安全层已接管时才分派到 TLS/SSL 写钩子。
                return WriteSecureSome(data, len);
            }

            return WriteSocketSome(data, len);
        }

        void TcpTransport::ShutdownWrite() {
            if (m_upgradeMode != TcpUpgradeMode::Disabled && SecureCommunicationReady()) {
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
            // 默认 TCP transport 没有连接级安全资源；只按配置决定是否立即升级。
            return ApplyConfiguredUpgradeMode();
        }

        IoResult TcpTransport::UpgradeCommunication() {
            if (m_upgradeMode == TcpUpgradeMode::Disabled) {
                // Disabled 明确选择最高性能普通 socket 路径，升级请求保持 no-op 兼容。
                return MakeOk(0);
            }

            // 默认不升级通信层，STARTTLS 等语义由用户派生类调用 BeginSecureHandshake 后定义。
            return MakeOk(0);
        }

        TcpUpgradeMode TcpTransport::UpgradeMode() const noexcept {
            return m_upgradeMode;
        }

        bool TcpTransport::NeedHandshake() const {
            if (m_upgradeMode == TcpUpgradeMode::Disabled) return false;
            return Transport::NeedHandshake();
        }

        IoResult TcpTransport::ApplyConfiguredUpgradeMode() {
            if (m_upgradeMode == TcpUpgradeMode::Disabled
                || m_upgradeMode == TcpUpgradeMode::Manual) {
                // Disabled/Manual 初始化期不进入握手；STARTTLS 由业务命令显式触发。
                return MakeOk(0);
            }

            if (m_upgradeMode == TcpUpgradeMode::Immediate
                || ShouldAutoUpgradeImmediately()) {
                // Immediate 固定立即升级；Auto 可由派生类按端口/配置选择同样路径。
                return UpgradeCommunication();
            }

            return MakeOk(0);
        }

        bool TcpTransport::ShouldAutoUpgradeImmediately() const noexcept {
            // 基础 Net 不知道 SMTP/IMAP/POP3 端口语义，默认让 Auto 等待首包或业务命令。
            return false;
        }

        bool TcpTransport::ShouldAutoUpgradeFromPeekedBytes(
            const std::uint8_t* data,
            std::size_t len) const noexcept {
            if (data == nullptr || len < 3) return false;

            const bool tlsHandshakeRecord = data[0] == 0x16; // TLS ClientHello/ServerHello record
            const bool tlsRecordMajor = data[1] == 0x03;     // SSLv3/TLS 的 record major version
            const bool tlsRecordMinor = data[2] <= 0x04;     // TLS 1.3 仍常用 0x03 record 版本
            return tlsHandshakeRecord && tlsRecordMajor && tlsRecordMinor;
        }

        IoResult TcpTransport::TryAutoUpgradeFromSocket() {
            std::array<std::uint8_t, 5> peekBuffer{}; // TLS record header 至少 5 字节，3 字节足够判别

            for (;;) {
                const SocketType fd = CurrentSocket(); // Auto 判别只偷看当前 socket，不消费数据
                if (fd == kInvalidSocket) return MakeOk(0);

                const std::int64_t rc = Internal::ReceiveSocket(
                    fd,
                    peekBuffer.data(),
                    peekBuffer.size(),
                    MSG_PEEK); // 平台层统一 Winsock/POSIX 缓冲类型

                if (rc > 0) {
                    if (ShouldAutoUpgradeFromPeekedBytes(
                        peekBuffer.data(),
                        static_cast<std::size_t>(rc))) {
                        // 识别到直连 TLS 首包后，仍交给用户派生 UpgradeCommunication 建立安全层。
                        return UpgradeCommunication();
                    }
                    return MakeOk(0);
                }

                if (rc == 0) return MakeOk(0);

                const int error = Internal::GetLastSocketError(); // 保存 errno/WSA 错误码
                if (Internal::IsInterrupted(error)) continue;
                if (Internal::IsWouldBlock(error)) return MakeOk(0);
                return MakeError(error);
            }
        }

        IoResult TcpTransport::ReadSocketSome(Buffer& in) {
            constexpr std::size_t kReadChunkSize = 8 * 1024; // 小请求避免为每条连接预留 64 KiB
            for (;;) {
                in.EnsureWritableBytes(kReadChunkSize); // 保留旧 8 KiB 公平读批次，同时继续直接写入 Buffer
                std::uint8_t* writeBegin = in.BeginWrite(); // socket 直接写入当前连续可写区
                const SocketType fd = CurrentSocket(); // 本轮读取使用的 socket 快照
                const std::int64_t rc = Internal::ReceiveSocket(
                    fd,
                    writeBegin,
                    kReadChunkSize,
                    0); // 平台层统一长度上限与缓冲类型

                if (rc > 0) {
                    in.HasWritten(static_cast<std::size_t>(rc));
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

            for (;;) {
                const SocketType fd = CurrentSocket(); // 本轮写入使用的 socket 快照
                const std::int64_t rc = Internal::SendSocket(
                    fd,
                    data,
                    len,
                    0); // 平台层统一单次发送长度与缓冲类型

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
