#pragma once
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Transport.hpp>

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API UdpTransport : public Transport {
        public:
            // 接管一个 UDP socket。
            explicit UdpTransport(SocketType fd);
            // 析构时关闭仍被拥有的 socket。
            ~UdpTransport() override;

            // 返回 UDP 明文传输族。
            TransportKind Kind() const noexcept override {
                return TransportKind::Udp;
            }
            // 从 UDP socket 读取一个数据报，并记录最近发送方。
            IoResult ReadSome(Buffer& in) final;
            // connected UDP 直接 send；服务端未 connect 时向最近发送方 sendto。
            IoResult WriteSome(const std::uint8_t* data, std::size_t len) final;
            // UDP 没有 TCP 半关闭语义，此函数保持无副作用。
            void ShutdownWrite() final;
            // 关闭 UDP socket。
            void Close() final;
            // 初始化本连接安全层资源，默认 UDP 明文无需额外动作且不进入握手态。
            IoResult InitializeSecureLayer() override;
            // 升级通信层，派生类可在这里显式切入安全层握手态。
            IoResult UpgradeCommunication() override;
            // 返回是否已记录最近发送方。
            bool HasLastPeer() const noexcept;
            // 返回最近发送方地址；没有发送方时返回无效地址。
            Address LastPeerAddress() const;

        protected:
            // 普通 socket 读钩子，派生类未重载时保留 UDP 默认读能力。
            virtual IoResult ReadSocketSome(Buffer& in);
            // 普通 socket 写钩子，派生类未重载时保留 UDP 默认写能力。
            virtual IoResult WriteSocketSome(const std::uint8_t* data, std::size_t len);
            // 普通 UDP 没有半关闭语义，默认保持空操作。
            virtual void ShutdownSocketWrite();
            // 普通 socket 关闭钩子。
            virtual void CloseSocket();
            // 安全层读钩子，默认回退到普通 socket 读。
            virtual IoResult ReadSecureSome(Buffer& in);
            // 安全层写钩子，默认回退到普通 socket 写。
            virtual IoResult WriteSecureSome(const std::uint8_t* data, std::size_t len);
            // 安全层半关闭钩子，默认回退到普通 UDP 空操作。
            virtual void ShutdownSecureWrite();
            // 安全层资源释放钩子，默认无额外资源。
            virtual void CloseSecureLayer();

        private:
            struct UdpTransportImpl;

            UdpTransportImpl* m_impl = nullptr; // UDP 实现，隐藏最近发送方缓存
        };
    }
}
