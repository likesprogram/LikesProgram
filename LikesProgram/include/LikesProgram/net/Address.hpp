#pragma once
#include "SocketType.hpp"
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

namespace LikesProgram {
    namespace Net {
        class Address {
        public:
            enum class Family {
                IPv4 = AF_INET,
                IPv6 = AF_INET6,
                Unspec = AF_UNSPEC
            };

            Address() = default;

            explicit Address(const sockaddr_storage& addr);

            Address(const std::string& ip, uint16_t port);

            const std::string& Ip() const noexcept;
            uint16_t Port() const noexcept;

            std::string ToString() const noexcept;

            const sockaddr* SockAddr() const noexcept;

            socklen_t Length() const noexcept;

            int FamilyValue() const noexcept;

            // DNS 解析
            static std::vector<Address> Resolve(const std::string& host, uint16_t port, Family family = Family::Unspec);

            // 获取对端地址
            static Address GetRemoteAddress(SocketType fd);

            // 获取本端地址
            static Address GetLocalAddress(SocketType fd);
        private:
            sockaddr_storage m_addr{};
            std::string m_ip;
            uint16_t m_port = 0;
        };
    }
}