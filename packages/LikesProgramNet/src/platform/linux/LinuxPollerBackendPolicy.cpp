#include "net/platform/linux/LinuxPollerBackendPolicy.hpp"
#include <cstring>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            LinuxPollerBackendRequest ParseLinuxPollerBackendRequest(
                const char* value) noexcept {
                // 仅接受精确的小写请求值，其他输入保持自动选择。
                if (!value) return LinuxPollerBackendRequest::Auto;
                if (std::strcmp(value, "auto") == 0) return LinuxPollerBackendRequest::Auto;
                if (std::strcmp(value, "io_uring") == 0) return LinuxPollerBackendRequest::IoUring;
                if (std::strcmp(value, "epoll") == 0) return LinuxPollerBackendRequest::Epoll;
                return LinuxPollerBackendRequest::Auto;
            }

            LinuxPollerBackend SelectLinuxPollerBackend(
                LinuxPollerBackendRequest request,
                bool ioUringReady) noexcept {
                // 显式请求不受构造期探测结果影响。
                if (request == LinuxPollerBackendRequest::IoUring) {
                    return LinuxPollerBackend::IoUring;
                }
                if (request == LinuxPollerBackendRequest::Epoll) {
                    return LinuxPollerBackend::Epoll;
                }

                // 自动模式只在 io_uring 可用时选用主后端。
                return ioUringReady
                    ? LinuxPollerBackend::IoUring
                    : LinuxPollerBackend::Epoll;
            }
        }
    }
}
