#pragma once
#include "LikesProgramLibExport.hpp"
#include <chrono>
#include <string>
#include <atomic>
#include <thread>

namespace LikesProgram {
    // 高精度计时器
    class LIKESPROGRAM_API Timer {
    public:
        using Duration = std::chrono::nanoseconds;
        explicit Timer(bool autoStart = false);
        
        // 开始计时
        static void Start();

        // 停止计时
        static Duration Stop(double alpha = 0.9);

        // 重置计时器
        static void ResetThread();
        static void ResetGlobal();

        // 最近一次 Stop() 的耗时
        static Duration GetLastElapsed();

        // 线程 Stop() 的累计耗时
        static Duration GetTotalElapsed();

        // 全局 Stop() 的累计耗时
        static Duration GetTotalGlobalElapsed();

        // 历史最长耗时
        static Duration GetLongestElapsed();

        // EMA 平均耗时
        static Duration GetEMAAverageElapsed();

        // 算数平均耗时 (线程内)
        static Duration GetArithmeticAverageElapsed();

        // 算数平均耗时 (全局)
        static Duration GetArithmeticAverageGlobalElapsed();

        // 当前是否在计时
        static bool IsRunning();

        // 将时间间隔转换为字符串
        static std::string ToString(Duration duration);
    private:
        // 获取高精度纳秒时间
        static int64_t NowNs();

        // 线程局部计时器
        thread_local static inline int64_t m_startNs = 0;
        thread_local static inline int64_t m_lastNs = 0;
        thread_local static inline int64_t m_totalNs = 0;
        thread_local static inline int64_t m_count = 0;
        thread_local static inline bool m_running = false;

        // 全局计时器 (多线程共享)
        static inline std::atomic<long long> totalNs_ { 0 }; // 全局累计时间
        static inline std::atomic<long long> count_{ 0 }; // 总计时次数
        static inline std::atomic<long long> longestNs_ { 0 }; // 全局最长时间
        static inline std::atomic<double> averageNs_ { 0 }; // 平均时间
    };
}
