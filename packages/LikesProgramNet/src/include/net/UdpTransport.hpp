#pragma once
#include <LikesProgram/Net/Address.hpp>
#include "net/Transport.hpp"

namespace LikesProgram {
    namespace Net {
        struct UdpReceiveDatagram {
            Buffer* buffer = nullptr;   // 接收目标 Buffer，由调用方持有并复用容量。
            Address* peer = nullptr;    // 可选输出发送方地址，允许调用方避免依赖最近 peer 缓存。
            IoResult result{};          // 单条 datagram 的接收结果，批量调用后写入。
        };

        struct UdpSendDatagram {
            const Address* peer = nullptr;      // 目标 peer，批量发送要求显式指定。
            const std::uint8_t* data = nullptr; // 待发送字节块，生命周期覆盖本次调用即可。
            std::size_t len = 0;                // 待发送字节数，0 表示无需发送且返回成功。
            IoResult result{};                  // 单条 datagram 的发送结果，批量调用后写入。
        };

        class UdpTransport : public Transport {
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
            // 向指定 peer 发送单个 UDP datagram，不依赖最近发送方缓存。
            IoResult SendTo(const Address& peer, const std::uint8_t* data, std::size_t len);
            // 批量读取 UDP datagram；Linux 优先使用 recvmmsg，其他平台保持同语义循环读取。
            IoResult ReadBatch(UdpReceiveDatagram* datagrams, std::size_t count);
            // 批量向显式 peer 发送 UDP datagram；Linux 优先使用 sendmmsg，其他平台保持同语义循环发送。
            IoResult SendBatch(UdpSendDatagram* datagrams, std::size_t count);
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
            // 普通 socket 指定 peer 写钩子，默认使用 sendto 发送一个 datagram。
            virtual IoResult SendSocketTo(const Address& peer, const std::uint8_t* data, std::size_t len);
            // 普通 socket 批量读钩子，派生类可接入平台专用批量收包。
            virtual IoResult ReadSocketBatch(UdpReceiveDatagram* datagrams, std::size_t count);
            // 普通 socket 批量写钩子，派生类可接入平台专用批量发包。
            virtual IoResult SendSocketBatch(UdpSendDatagram* datagrams, std::size_t count);
            // 普通 UDP 没有半关闭语义，默认保持空操作。
            virtual void ShutdownSocketWrite();
            // 普通 socket 关闭钩子。
            virtual void CloseSocket();
            // 安全层读钩子，默认回退到普通 socket 读。
            virtual IoResult ReadSecureSome(Buffer& in);
            // 安全层写钩子，默认回退到普通 socket 写。
            virtual IoResult WriteSecureSome(const std::uint8_t* data, std::size_t len);
            // 安全层指定 peer 写钩子，默认回退到普通 socket sendto。
            virtual IoResult SendSecureTo(const Address& peer, const std::uint8_t* data, std::size_t len);
            // 安全层批量读钩子，默认回退到普通 UDP 批量读。
            virtual IoResult ReadSecureBatch(UdpReceiveDatagram* datagrams, std::size_t count);
            // 安全层批量写钩子，默认回退到普通 UDP 批量写。
            virtual IoResult SendSecureBatch(UdpSendDatagram* datagrams, std::size_t count);
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
