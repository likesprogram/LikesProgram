#include "threading/ThreadName.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <pthread.h>
#endif

#include <string>

namespace LikesProgram {
    namespace Threading {
        namespace Detail {
            void SetCurrentThreadName(const String& name) noexcept {
                try {
                    const std::string utf8 = name.ToStdString(); // 平台 API 使用窄字符线程名
                    if (utf8.empty()) return;

#if defined(_WIN32)
                    // Windows 10+ 支持 SetThreadDescription，失败不影响线程池语义。
                    const std::wstring wide = name.ToWString();
                    (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
#elif defined(__APPLE__)
                    // macOS 限制当前线程命名，pthread 会截断过长名称。
                    (void)pthread_setname_np(utf8.substr(0, 63).c_str());
#elif defined(__linux__) || defined(__unix__)
                    // Linux pthread name 最多 15 字节加 NUL，提前截断避免 ERANGE。
                    (void)pthread_setname_np(pthread_self(), utf8.substr(0, 15).c_str());
#else
                    (void)utf8;
#endif
                }
                catch (...) {
                    // 线程命名是诊断增强，不能影响 worker 生命周期。
                }
            }
        }
    }
}
