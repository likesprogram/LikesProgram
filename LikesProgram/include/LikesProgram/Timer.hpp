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
        explicit Timer(bool autoStart = false, Timer* parent = nullptr);
        void SetParent(Timer* parent);
        ~Timer() = default;
        
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

        std::atomic<int64_t> m_startNs = 0;
        std::atomic<int64_t> m_lastNs = 0;
        std::atomic<bool> m_running = false;
        std::atomic<long long> m_totalNs{ 0 };
        std::atomic<long long> m_count{ 0 };
        std::atomic<long long> m_longestNs{ 0 };
        std::atomic<double> m_averageNs{ 0 };

        mutable std::shared_mutex m_mutex;
    };
}
