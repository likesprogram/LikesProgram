#include "../../../../include/LikesProgram/net/transports/Transport.hpp"
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        SocketType Transport::DetachFd() noexcept {
            SocketType fd = m_fd;
            m_fd = kInvalidSocket;
            m_closed.store(true, std::memory_order_release);
            return fd;
        }
        IoResult Transport::MakeOk(int64_t n) {
            return { IoStatus::Ok, n, 0 };
        }
        IoResult Transport::MakeWouldBlock() {
            return { IoStatus::WouldBlock, 0, 0 };
        }
        IoResult Transport::MakePeerClosed() {
            return { IoStatus::PeerClosed, 0, 0 };
        }
        IoResult Transport::MakeError(int err) {
            return { IoStatus::Error, 0, err };
        }
    }
}
