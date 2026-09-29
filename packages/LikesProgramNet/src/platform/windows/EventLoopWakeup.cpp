#include "net/platform/EventLoopWakeup.hpp"
#include "net/platform/SocketOps.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            EventLoopWakeup::EventLoopWakeup() {
                const SocketType readSocket =
                    CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                const SocketType writeSocket =
                    CreateSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (readSocket == kInvalidSocket || writeSocket == kInvalidSocket) {
                    CloseSocket(readSocket);
                    CloseSocket(writeSocket);
                    return;
                }

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = 0;
                SocketLength length = static_cast<SocketLength>(sizeof(address));
                if (BindSocket(
                        readSocket,
                        reinterpret_cast<const sockaddr*>(&address),
                        length) != 0
                    || ::getsockname(
                        readSocket,
                        reinterpret_cast<sockaddr*>(&address),
                        &length) != 0
                    || ConnectSocket(
                        writeSocket,
                        reinterpret_cast<const sockaddr*>(&address),
                        length) != 0
                    || !SetNonBlocking(readSocket)
                    || !SetNonBlocking(writeSocket)) {
                    CloseSocket(readSocket);
                    CloseSocket(writeSocket);
                    return;
                }

                m_readSocket = readSocket;
                m_writeSocket = writeSocket;
            }

            EventLoopWakeup::~EventLoopWakeup() {
                CloseSocket(m_readSocket);
                CloseSocket(m_writeSocket);
                m_readSocket = kInvalidSocket;
                m_writeSocket = kInvalidSocket;
            }

            bool EventLoopWakeup::IsValid() const noexcept {
                return m_readSocket != kInvalidSocket
                    && m_writeSocket != kInvalidSocket;
            }

            SocketType EventLoopWakeup::ReadSocket() const noexcept {
                return m_readSocket;
            }

            bool EventLoopWakeup::Wakeup() noexcept {
                if (!IsValid()) return false;
                const std::uint8_t value = 1;
                const std::int64_t result = SendSocket(
                    m_writeSocket,
                    &value,
                    sizeof(value),
                    0);
                if (result >= 0) return true;
                const int error = GetLastSocketError();
                return IsWouldBlock(error) || IsInterrupted(error);
            }

            void EventLoopWakeup::Drain() noexcept {
                if (!IsValid()) return;
                std::uint8_t value[64]{};
                for (;;) {
                    const std::int64_t result = ReceiveSocket(
                        m_readSocket,
                        value,
                        sizeof(value),
                        0);
                    if (result > 0) continue;
                    if (result == 0) return;
                    const int error = GetLastSocketError();
                    if (IsWouldBlock(error) || IsInterrupted(error)) return;
                    return;
                }
            }
        }
    }
}

#endif
