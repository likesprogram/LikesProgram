#include "net/platform/windows/WindowsPollerBackendPolicy.hpp"

#include <cstring>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            WindowsPollerBackendRequest ParseWindowsPollerBackendRequest(
                const char* value) noexcept {
                // 空值、auto、iocp 和未知文本都保持 Windows 原生后端。
                if (value == nullptr || *value == '\0'
                    || std::strcmp(value, "auto") == 0
                    || std::strcmp(value, "iocp") == 0) {
                    return WindowsPollerBackendRequest::Iocp;
                }
                // 已知外来后端必须显式失败，避免误用 Linux/BSD 环境变量。
                if (std::strcmp(value, "io_uring") == 0
                    || std::strcmp(value, "epoll") == 0
                    || std::strcmp(value, "kqueue") == 0) {
                    return WindowsPollerBackendRequest::UnsupportedKnown;
                }
                return WindowsPollerBackendRequest::Iocp;
            }
        }
    }
}
