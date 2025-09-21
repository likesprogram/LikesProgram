#pragma once
#include "LikesProgramLibExport.hpp"
#include <chrono>
#include "String.hpp"
#include <atomic>
#include <shared_mutex>

namespace LikesProgram {
    // 高精度计时器
    class LIKESPROGRAM_API Timer {
    public:
        using Duration = std::chrono::nanoseconds;
        // 构造函数, 默认不启动, 父计时器为空
        explicit Timer(bool autoStart = false, Timer* parent = nullptr);

        // 设置父计时器
        void SetParent(Timer* parent);
        ~Timer();

        // 拷贝构造 & 拷贝赋值
        Timer(const Timer& other);
        Timer& operator=(const Timer& other);

        // 移动构造 & 移动赋值
        Timer(Timer&& other) noexcept;
        Timer& operator=(Timer&& other) noexcept;

        // 开始计时
        void Start();

        // 停止计时
        Duration Stop(double alpha = 0.9);

        // 重置计时器
        void ResetThread();
        void ResetGlobal();
        void Reset();

        // 最近一次 Stop() 的耗时
        Duration GetLastElapsed() const;

        // 全局 Stop() 的累计耗时
        Duration GetTotalElapsed() const;

        // 历史最长耗时
        Duration GetLongestElapsed() const;

        // EMA 平均耗时
        Duration GetEMAAverageElapsed() const;

        // 算数平均耗时
        Duration GetArithmeticAverageElapsed() const;

        // 当前是否在计时
        bool IsRunning() const;

        // 将时间间隔转换为字符串
        static String ToString(Duration duration);
    private:
        // 获取高精度纳秒时间
        static int64_t NowNs();
        Timer* m_parent = nullptr;

        struct TimerImpl;
        TimerImpl* m_impl = nullptr;
    };
}
