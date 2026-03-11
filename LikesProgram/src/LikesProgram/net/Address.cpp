#include "../../../include/LikesProgram/net/Address.hpp"
#include <cstring>

namespace LikesProgram {
    namespace Net {
        Address::Address(const sockaddr_storage& addr) : m_addr(addr) {
            char buf[INET6_ADDRSTRLEN]{};

            if (m_addr.ss_family == AF_INET) {

                auto* addr4 = (sockaddr_in*)&m_addr;

                inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
                m_port = ntohs(addr4->sin_port);
            }
            else if (m_addr.ss_family == AF_INET6) {

                auto* addr6 = (sockaddr_in6*)&m_addr;

                inet_ntop(AF_INET6, &addr6->sin6_addr, buf, sizeof(buf));
                m_port = ntohs(addr6->sin6_port);
            }

            if (buf[0]) m_ip = buf;
            else m_ip = "unknown";
        }

        Address::Address(const std::string& ip, uint16_t port) : m_ip(ip), m_port(port) {
            memset(&m_addr, 0, sizeof(m_addr));

            sockaddr_in addr4{};
            if (inet_pton(AF_INET, ip.c_str(), &addr4.sin_addr) == 1) {
                addr4.sin_family = AF_INET;
                addr4.sin_port = htons(port);
                memcpy(&m_addr, &addr4, sizeof(addr4));
                return;
            }

            sockaddr_in6 addr6{};
            if (inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr) == 1) {
                addr6.sin6_family = AF_INET6;
                addr6.sin6_port = htons(port);
                memcpy(&m_addr, &addr6, sizeof(addr6));
            }
        }


        const std::string& Address::Ip() const noexcept {
            return m_ip;
        }
        uint16_t Address::Port() const noexcept {
            return m_port;
        }

        std::string Address::ToString() const noexcept {
            if (FamilyValue() == AF_INET6) return "[" + m_ip + "]:" + std::to_string(m_port);
            return m_ip + ":" + std::to_string(m_port);
        }

        const sockaddr* Address::SockAddr() const noexcept {
            return reinterpret_cast<const sockaddr*>(&m_addr);
        }

        socklen_t Address::Length() const noexcept {
            if (m_addr.ss_family == AF_INET)
                return sizeof(sockaddr_in);
            if (m_addr.ss_family == AF_INET6)
                return sizeof(sockaddr_in6);
            return sizeof(sockaddr_storage);
        }

        int Address::FamilyValue() const noexcept {
            return m_addr.ss_family;
        }

        std::vector<Address> Address::Resolve(const std::string& host, uint16_t port, Family family) {
            std::vector<Address> out;

            addrinfo hints{};
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_family = (int)family;

            const char* node = nullptr;
            if (host.empty() || host == "*") {
                hints.ai_flags = AI_PASSIVE;
                node = nullptr;
            }
            else node = host.c_str();

            addrinfo* result = nullptr;

            std::string portStr = std::to_string(port);

            int gai = ::getaddrinfo(node, portStr.c_str(), &hints, &result);

            if (gai != 0 || !result) return out;

            for (addrinfo* rp = result; rp; rp = rp->ai_next) {
                sockaddr_storage addr{};
                memcpy(&addr, rp->ai_addr, rp->ai_addrlen);

                out.emplace_back(addr);
            }

            freeaddrinfo(result);
            return out;
        }

        // 获取对端地址
        Address Address::GetRemoteAddress(SocketType fd) {
            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            if (::getpeername(fd, (sockaddr*)&addr, &len) == 0) return Address(addr);
            return {};
        }

        // 获取本端地址
        Address Address::GetLocalAddress(SocketType fd) {
            sockaddr_storage addr{};
            socklen_t len = sizeof(addr);
            if (::getsockname(fd, (sockaddr*)&addr, &len) == 0) return Address(addr);
            return {};
        }
    }
}