#pragma once
#include <LikesProgram/Net/Transport.hpp>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API TcpTransport : public Transport {
        public:
            // 接管一个 TCP socket。
            explicit TcpTransport(SocketType fd);
            // 析构时关闭仍被拥有的 socket。
            ~TcpTransport() override;

            // 返回 TCP 明文传输族。
            TransportKind Kind() const noexcept override {
                return TransportKind::Tcp;
            }
            // 从 TCP socket 读取一批数据到输入缓冲。
            IoResult ReadSome(Buffer& in) final;
            // 将内存中的数据写入 TCP socket。
            IoResult WriteSome(const std::uint8_t* data, std::size_t len) final;
            // 关闭 TCP 写端。
            void ShutdownWrite() final;
            // 关闭 TCP socket。
            void Close() final;
            // 初始化本连接安全层资源，默认 TCP 明文无需额外动作且不进入握手态。
            IoResult InitializeSecureLayer() override;
            // 升级通信层，派生类可在这里显式切入安全层握手态。
            IoResult UpgradeCommunication() override;

        protected:
            // 普通 socket 读钩子，派生类未重载时保留 TCP 默认读能力。
            virtual IoResult ReadSocketSome(Buffer& in);
            // 普通 socket 写钩子，派生类未重载时保留 TCP 默认写能力。
            virtual IoResult WriteSocketSome(const std::uint8_t* data, std::size_t len);
            // 普通 socket 半关闭写端。
            virtual void ShutdownSocketWrite();
            // 普通 socket 关闭钩子。
            virtual void CloseSocket();
            // 安全层读钩子，默认回退到普通 socket 读。
            virtual IoResult ReadSecureSome(Buffer& in);
            // 安全层写钩子，默认回退到普通 socket 写。
            virtual IoResult WriteSecureSome(const std::uint8_t* data, std::size_t len);
            // 安全层半关闭钩子，默认回退到普通 socket 半关闭。
            virtual void ShutdownSecureWrite();
            // 安全层资源释放钩子，默认无额外资源。
            virtual void CloseSecureLayer();
        };
    }
}
