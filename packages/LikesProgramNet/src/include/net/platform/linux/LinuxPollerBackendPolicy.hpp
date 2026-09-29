#pragma once

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class LinuxPollerBackendRequest {
                Auto,
                IoUring,
                Epoll
            };

            enum class LinuxPollerBackend {
                IoUring,
                Epoll
            };

            // 解析默认工厂的进程级后端请求。
            LinuxPollerBackendRequest ParseLinuxPollerBackendRequest(
                const char* value) noexcept;
            // 根据请求与 io_uring 构造期可用性选择最终后端。
            LinuxPollerBackend SelectLinuxPollerBackend(
                LinuxPollerBackendRequest request,
                bool ioUringReady) noexcept;
        }
    }
}
