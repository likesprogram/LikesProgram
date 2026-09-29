#pragma once
#include <LikesProgram/Core/Status.hpp>
#include <optional>
#include <stdexcept>
#include <utility>

namespace LikesProgram {
    template <typename T>
    // 携带值或状态的返回契约，适合扩展包表达可恢复失败。
    class Result {
    public:
        // 延迟失败诊断的工厂：数值上下文自包含，短语必须具有静态存储期。
        using LazyStatusFactory = Status(*)(size_t primaryContext, size_t secondaryContext, const char16_t* staticMessage);

        // 从值构造成功结果。
        Result(const T& value) : m_value(value) { }

        // 从值移动构造成功结果。
        Result(T&& value) : m_value(std::move(value)) { }

        // 从失败状态构造结果；传入 Ok 会转换为内部错误以避免无值成功。
        Result(const Status& status) : m_status(NormalizeStatus(status)) { }

        // 构造延迟诊断的失败结果；两个数值的语义由对应工厂解释。
        static Result LazyFailure(
            LazyStatusFactory factory,
            size_t primaryContext,
            size_t secondaryContext,
            const char16_t* staticMessage) {
            Result result;
            result.m_lazyFactory = factory;
            result.m_lazyPrimaryContext = primaryContext;
            result.m_lazySecondaryContext = secondaryContext;
            result.m_lazyMessage = staticMessage;
            return result;
        }

        // 拷贝构造保持当前分支。
        Result(const Result& other) = default;

        // 移动构造保持当前分支。
        Result(Result&& other) noexcept = default;

        // 拷贝赋值先销毁当前分支，再重建目标分支。
        Result& operator=(const Result& other) = default;

        // 移动赋值先销毁当前分支，再重建目标分支。
        Result& operator=(Result&& other) noexcept = default;

        // 析构当前活跃分支。
        ~Result() = default;

        // 当前结果是否成功并持有值；延迟失败不物化诊断。
        bool IsOk() const { return m_value.has_value(); }

        // 允许在 if(result) 中表达成功判断。
        explicit operator bool() const { return IsOk(); }

        // 返回结果状态；首次访问时才执行延迟诊断工厂。
        const Status& GetStatus() const {
            static const Status okStatus; // 成功状态共享单例，成功路径不保存 Status
            if (IsOk()) return okStatus;
            ResolveLazyStatus();
            return m_status;
        }

        // 将失败分支转到另一结果类型，且不触发诊断物化。
        template <typename U>
        Result<U> PropagateFailure() const {
            if (IsOk()) return Result<U>(Status::Internal(u"PropagateFailure on successful Result"));
            if (m_lazyFactory != nullptr && !m_lazyResolved) return Result<U>::LazyFailure(m_lazyFactory, m_lazyPrimaryContext, m_lazySecondaryContext, m_lazyMessage);
            return Result<U>(m_status);
        }

        // 返回可修改值；失败时抛出 runtime_error。
        T& Value() {
            if (!IsOk()) throw std::runtime_error(GetStatus().ToString().ToStdString());
            return *m_value;
        }

        // 返回只读值；失败时抛出 runtime_error。
        const T& Value() const {
            if (!IsOk()) throw std::runtime_error(GetStatus().ToString().ToStdString());
            return *m_value;
        }

        // 移出结果值；失败时抛出 runtime_error。
        T&& MoveValue() {
            if (!IsOk()) throw std::runtime_error(GetStatus().ToString().ToStdString());
            return std::move(*m_value);
        }

        // 成功时返回值，否则返回调用方提供的默认值。
        T ValueOr(T fallback) const {
            if (!IsOk()) return fallback;
            return *m_value;
        }

    private:
        // 供 LazyFailure 使用的空失败壳。
        Result() = default;

        // 统一修正无值成功状态，避免 Result<T> 出现无值 Ok。
        static Status NormalizeStatus(const Status& status) {
            return status.IsOk() ? Status::Internal(u"Result success without value") : status;
        }

        // 物化延迟诊断；已物化或非延迟失败时为空操作。
        void ResolveLazyStatus() const {
            if (m_lazyFactory == nullptr || m_lazyResolved) return;
            m_status = m_lazyFactory(m_lazyPrimaryContext, m_lazySecondaryContext, m_lazyMessage);
            if (m_status.IsOk()) m_status = Status::Internal(u"Lazy Status factory returned Ok");
            m_lazyResolved = true;
        }

        std::optional<T> m_value; // 成功分支携带的值，失败时保持空状态。
        mutable Status m_status;  // 失败分支状态；延迟失败在首次 GetStatus 前保持默认 Ok。
        LazyStatusFactory m_lazyFactory = nullptr; // 非空表示尚未物化的语法失败诊断
        size_t m_lazyPrimaryContext = 0;           // 工厂解释的第一数值上下文
        size_t m_lazySecondaryContext = 0;         // 工厂解释的第二数值上下文
        const char16_t* m_lazyMessage = nullptr;   // 具有静态存储期的诊断短语
        mutable bool m_lazyResolved = false;       // 延迟诊断是否已写入 m_status
    };

    template <>
    // void 结果专用于只需要状态、不需要值的操作。
    class Result<void> {
    public:
        // 默认构造表示成功。
        Result() = default;

        // 从状态构造 void 结果。
        Result(const Status& status) : m_status(status) { }

        // 当前结果是否成功。
        bool IsOk() const { return m_status.IsOk(); }

        // 允许在 if(result) 中表达成功判断。
        explicit operator bool() const { return IsOk(); }

        // 返回结果状态。
        const Status& GetStatus() const { return m_status; }

        // 成功时无操作，失败时抛出 runtime_error。
        void Value() const {
            if (!IsOk()) throw std::runtime_error(m_status.ToString().ToStdString());
        }

    private:
        Status m_status; // void 结果只保存状态
    };
}
