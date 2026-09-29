#pragma once

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class WindowsPollerBackendRequest {
                Iocp,
                UnsupportedKnown
            };

            // 解析 Windows 原生 backend 请求，外来后端由工厂明确拒绝。
            WindowsPollerBackendRequest ParseWindowsPollerBackendRequest(
                const char* value) noexcept;
        }
    }
}
