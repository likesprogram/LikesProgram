#pragma once
#include <LikesProgram/Core/String.hpp>

namespace LikesProgram {
    namespace Threading {
        namespace Detail {
            // 尝试设置当前线程名；平台或运行时不支持时静默忽略。
            void SetCurrentThreadName(const String& name) noexcept;
        }
    }
}
