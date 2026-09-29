#include "net/platform/UdpBatchOps.hpp"

#ifdef _WIN32

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            bool TryReceiveUdpBatch(
                SocketType,
                UdpReceiveDatagram*,
                std::size_t,
                sockaddr_storage*,
                SocketLength*,
                IoResult*) {
                // Windows 首版由通用循环回退，真正 overlapped UDP 在 Task 8 接管。
                return false;
            }

            bool TrySendUdpBatch(
                SocketType,
                UdpSendDatagram*,
                std::size_t,
                IoResult*) {
                // Windows 首版由通用循环回退，真正 overlapped UDP 在 Task 8 接管。
                return false;
            }
        }
    }
}

#endif
