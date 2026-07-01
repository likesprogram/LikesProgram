#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 6101)
#endif
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace LikesProgram {
    namespace Net {
        class LIKESPROGRAM_NET_API Address {
        public:
            enum class Family {
                IPv4 = AF_INET,
                IPv6 = AF_INET6,
                Unspec = AF_UNSPEC
            };

            // 创建未指定地址，主要作为失败回退或延迟赋值占位。
            Address();
            // 从系统 sockaddr_storage 拷贝地址，并同步解析文本与端口缓存。
            explicit Address(const sockaddr_storage& addr, SocketLength length = sizeof(sockaddr_storage));
            // 从 IP 文本和端口创建地址，支持 IPv4/IPv6 字面量。
            Address(const std::string& ip, std::uint16_t port);
            // 复制地址快照，保持值类型语义。
            Address(const Address& other);
            // 移动地址快照，源对象进入空地址状态。
            Address(Address&& other) noexcept;
            // 释放内部地址缓存。
            ~Address();

            // 复制赋值地址快照。
            Address& operator=(const Address& other);
            // 移动赋值地址快照。
            Address& operator=(Address&& other) noexcept;

            // 返回规范化后的 IP 文本。
            const std::string& Ip() const noexcept;
            // 返回主机字节序端口。
            std::uint16_t Port() const noexcept;
            // 返回当前地址是否可用于 bind/connect。
            bool IsValid() const noexcept;
            // 返回 ip:port 形式，IPv6 会使用 [addr]:port。
            std::string ToString() const;

            // 返回只读 sockaddr 指针，供系统调用使用。
            const sockaddr* SockAddr() const noexcept;
            // 返回可写 sockaddr 指针，供 getsockname/getpeername 填充。
            sockaddr* SockAddr() noexcept;
            // 返回 sockaddr 实际长度。
            SocketLength Length() const noexcept;
            // 返回 AF_INET/AF_INET6/AF_UNSPEC。
            int FamilyValue() const noexcept;

            // DNS 解析 host:port，返回所有可连接候选地址。
            static std::vector<Address> Resolve(
                const std::string& host,
                std::uint16_t port,
                Family family = Family::Unspec);

            // 获取 socket 对端地址，失败时返回无效地址。
            static Address GetRemoteAddress(SocketType fd);
            // 获取 socket 本端地址，失败时返回无效地址。
            static Address GetLocalAddress(SocketType fd);

        private:
            struct AddressImpl;

            // 从 m_addr 重建 m_ip、m_port 和 m_valid 缓存。
            void RefreshText();

            AddressImpl* m_impl = nullptr;    // 地址实现缓存，避免导出类直接携带 std 成员
        };
    }
}
