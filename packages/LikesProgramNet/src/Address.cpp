#include <LikesProgram/Net/Address.hpp>
#include "net/SocketOps.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace LikesProgram {
    namespace Net {
        struct Address::AddressImpl {
            sockaddr_storage m_addr{};        // 系统地址结构，按实际 family 解释
            SocketLength m_length = 0;        // sockaddr 实际长度，0 表示无效地址
            std::string m_ip;                 // 规范化 IP 文本缓存
            std::uint16_t m_port = 0;         // 主机字节序端口缓存
            bool m_valid = false;             // 当前地址是否完整可用
        };

        namespace {
            const std::string& EmptyIpText() noexcept {
                static const std::string empty; // moved-from 地址的稳定空文本
                return empty;
            }
        }

        Address::Address()
            : m_impl(new AddressImpl{}) {
        }

        Address::Address(const sockaddr_storage& addr, SocketLength length)
            : m_impl(new AddressImpl{}) {
            m_impl->m_addr = addr;
            m_impl->m_length = length;
            RefreshText();
        }

        Address::Address(const std::string& ip, std::uint16_t port)
            : m_impl(new AddressImpl{}) {
            Internal::SocketRuntime::Ensure();

            sockaddr_in ipv4{}; // IPv4 候选，优先按字面量解析
            ipv4.sin_family = AF_INET;
            ipv4.sin_port = htons(port);
            if (::inet_pton(AF_INET, ip.c_str(), &ipv4.sin_addr) == 1) {
                std::memcpy(&m_impl->m_addr, &ipv4, sizeof(ipv4));
                m_impl->m_length = static_cast<SocketLength>(sizeof(ipv4));
                RefreshText();
                return;
            }

            sockaddr_in6 ipv6{}; // IPv6 候选，支持 ::1 等字面量
            ipv6.sin6_family = AF_INET6;
            ipv6.sin6_port = htons(port);
            if (::inet_pton(AF_INET6, ip.c_str(), &ipv6.sin6_addr) == 1) {
                std::memcpy(&m_impl->m_addr, &ipv6, sizeof(ipv6));
                m_impl->m_length = static_cast<SocketLength>(sizeof(ipv6));
                RefreshText();
                return;
            }

            auto resolved = Resolve(ip, port); // 非字面量时走 DNS 解析
            if (resolved.empty()) {
                throw std::invalid_argument("Address cannot resolve host: " + ip);
            }

            *this = resolved.front();
        }

        Address::Address(const Address& other)
            : m_impl(new AddressImpl{}) {
            if (other.m_impl) *m_impl = *other.m_impl;
        }

        Address::Address(Address&& other) noexcept
            : m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }

        Address::~Address() {
            delete m_impl;
            m_impl = nullptr;
        }

        Address& Address::operator=(const Address& other) {
            if (this == &other) return *this;

            if (!m_impl) m_impl = new AddressImpl{};
            if (other.m_impl) {
                *m_impl = *other.m_impl;
            }
            else {
                *m_impl = AddressImpl{};
            }
            return *this;
        }

        Address& Address::operator=(Address&& other) noexcept {
            if (this == &other) return *this;

            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        const std::string& Address::Ip() const noexcept {
            return m_impl ? m_impl->m_ip : EmptyIpText();
        }

        std::uint16_t Address::Port() const noexcept {
            return m_impl ? m_impl->m_port : 0;
        }

        bool Address::IsValid() const noexcept {
            return m_impl && m_impl->m_valid;
        }

        std::string Address::ToString() const {
            if (!IsValid()) return {};
            if (FamilyValue() == AF_INET6) return "[" + m_impl->m_ip + "]:" + std::to_string(m_impl->m_port);
            return m_impl->m_ip + ":" + std::to_string(m_impl->m_port);
        }

        const sockaddr* Address::SockAddr() const noexcept {
            return m_impl ? reinterpret_cast<const sockaddr*>(&m_impl->m_addr) : nullptr;
        }

        sockaddr* Address::SockAddr() noexcept {
            return m_impl ? reinterpret_cast<sockaddr*>(&m_impl->m_addr) : nullptr;
        }

        SocketLength Address::Length() const noexcept {
            return m_impl ? m_impl->m_length : 0;
        }

        int Address::FamilyValue() const noexcept {
            const sockaddr* addr = SockAddr(); // moved-from 地址没有系统结构
            if (addr == nullptr || Length() == 0) return AF_UNSPEC;
            return addr->sa_family;
        }

        std::vector<Address> Address::Resolve(
            const std::string& host,
            std::uint16_t port,
            Family family) {
            Internal::SocketRuntime::Ensure();

            addrinfo hints{}; // getaddrinfo 输入约束，保持 TCP stream 语义
            hints.ai_family = static_cast<int>(family);
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            addrinfo* rawResult = nullptr; // getaddrinfo 分配的链表，必须 freeaddrinfo
            const std::string service = std::to_string(port);
            const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &rawResult);
            if (rc != 0 || rawResult == nullptr) return {};

            std::vector<Address> result; // 解析出的候选地址
            for (addrinfo* item = rawResult; item != nullptr; item = item->ai_next) {
                if (item->ai_addr == nullptr || item->ai_addrlen > sizeof(sockaddr_storage)) {
                    continue;
                }

                sockaddr_storage storage{}; // 拷贝到稳定结构，脱离 getaddrinfo 生命周期
                std::memcpy(&storage, item->ai_addr, item->ai_addrlen);
                result.emplace_back(storage, static_cast<SocketLength>(item->ai_addrlen));
            }

            ::freeaddrinfo(rawResult);
            return result;
        }

        Address Address::GetRemoteAddress(SocketType fd) {
            sockaddr_storage storage{}; // getpeername 输出缓冲
            SocketLength length = static_cast<SocketLength>(sizeof(storage));
            if (::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
                return Address();
            }

            return Address(storage, length);
        }

        Address Address::GetLocalAddress(SocketType fd) {
            sockaddr_storage storage{}; // getsockname 输出缓冲
            SocketLength length = static_cast<SocketLength>(sizeof(storage));
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
                return Address();
            }

            return Address(storage, length);
        }

        void Address::RefreshText() {
            if (!m_impl) m_impl = new AddressImpl{};

            m_impl->m_valid = false;
            m_impl->m_ip.clear();
            m_impl->m_port = 0;

            std::array<char, INET6_ADDRSTRLEN> text{}; // inet_ntop 输出缓存
            if (FamilyValue() == AF_INET) {
                const auto* addr = reinterpret_cast<const sockaddr_in*>(&m_impl->m_addr);
                if (::inet_ntop(AF_INET, &addr->sin_addr, text.data(), static_cast<SocketLength>(text.size())) == nullptr) {
                    return;
                }
                m_impl->m_ip = text.data();
                m_impl->m_port = ntohs(addr->sin_port);
                m_impl->m_valid = true;
            }
            else if (FamilyValue() == AF_INET6) {
                const auto* addr = reinterpret_cast<const sockaddr_in6*>(&m_impl->m_addr);
                if (::inet_ntop(AF_INET6, &addr->sin6_addr, text.data(), static_cast<SocketLength>(text.size())) == nullptr) {
                    return;
                }
                m_impl->m_ip = text.data();
                m_impl->m_port = ntohs(addr->sin6_port);
                m_impl->m_valid = true;
            }
        }
    }
}
