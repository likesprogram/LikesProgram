#include "net/platform/EventLoopWakeup.hpp"
#include "net/platform/SocketOps.hpp"

#if defined(__linux__)

#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            EventLoopWakeup::EventLoopWakeup() {
                m_readSocket = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); // Linux 使用单 fd 计数唤醒
                m_writeSocket = m_readSocket;
            }

            EventLoopWakeup::~EventLoopWakeup() {
                if (m_readSocket != kInvalidSocket) CloseSocket(m_readSocket);
                m_readSocket = kInvalidSocket;
                m_writeSocket = kInvalidSocket;
            }

            bool EventLoopWakeup::IsValid() const noexcept {
                return m_readSocket != kInvalidSocket;
            }

            SocketType EventLoopWakeup::ReadSocket() const noexcept {
                return m_readSocket;
            }

            bool EventLoopWakeup::Wakeup() noexcept {
                if (!IsValid()) return false;

                const std::uint64_t value = 1; // eventfd 累加跨线程唤醒次数
                const auto result = ::write(m_writeSocket, &value, sizeof(value));
                return result >= 0 || IsWouldBlock(errno) || IsInterrupted(errno);
            }

            void EventLoopWakeup::Drain() noexcept {
                if (!IsValid()) return;

                for (;;) {
                    std::uint64_t value = 0; // 一次读取清空当前计数快照
                    const auto result = ::read(m_readSocket, &value, sizeof(value));
                    if (result > 0) continue;
                    if (result == 0 || IsWouldBlock(errno) || IsInterrupted(errno)) return;
                    return;
                }
            }
        }
    }
}

#endif
