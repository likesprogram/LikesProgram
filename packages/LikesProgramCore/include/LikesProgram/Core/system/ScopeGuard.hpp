#pragma once
#include <type_traits>
#include <utility>

namespace LikesProgram {
    // 禁止拷贝的基类，供资源拥有型对象复用。
    class NonCopyable {
    protected:
        NonCopyable() = default;
        ~NonCopyable() = default;
    public:
        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
    };

    template <typename F>
    // 作用域退出时执行回调，适合早返回路径清理资源。
    class ScopeGuard {
    public:
        // 保存退出回调并启用 guard。
        explicit ScopeGuard(F&& fn) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_fn(std::forward<F>(fn)), m_active(true) { }

        // 移动 guard，原 guard 失效。
        ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
            : m_fn(std::move(other.m_fn)), m_active(other.m_active) {
            other.Dismiss();
        }

        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
        ScopeGuard& operator=(ScopeGuard&&) = delete;

        // 析构时执行回调；回调异常会被吞掉以避免析构抛出。
        ~ScopeGuard() noexcept {
            if (!m_active) return;
            try {
                m_fn();
            }
            catch (...) {
                // 清理回调不得让析构路径继续抛出。
            }
        }

        // 取消退出回调。
        void Dismiss() noexcept {
            m_active = false;
        }

    private:
        F m_fn;              // 退出时执行的回调
        bool m_active = true; // 是否仍需要执行回调
    };

    // 根据回调类型推导 ScopeGuard。
    template <typename F>
    ScopeGuard<std::decay_t<F>> MakeScopeGuard(F&& fn) {
        return ScopeGuard<std::decay_t<F>>(std::forward<F>(fn));
    }
}
