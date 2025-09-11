#include "../../include/LikesProgram/Timer.hpp"
#include "../../include/LikesProgram/math/Math.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace LikesProgram {
    Timer::Timer(bool autoStart){
        if (autoStart) Start();
    }

    void Timer::Start(){
        m_startNs = NowNs();
        m_running = true;
    }

    Timer::Duration Timer::Stop(double alpha)
    {
        if (!m_running) return Duration(0);
        int64_t elapsedNs = NowNs() - m_startNs;
        m_lastNs = elapsedNs;

        // 线程局部累积
        m_totalNs += elapsedNs;
        m_count++;

        // 全局累积
        totalNs_.fetch_add(elapsedNs, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);

        // 更新最长耗时
        Math::UpdateMax(longestNs_, elapsedNs);

        // EMA 更新
        double prevAvg = averageNs_.load(std::memory_order_relaxed);
        double nextAvg = (prevAvg == 0.0) ? static_cast<double>(elapsedNs) : Math::EMA(prevAvg, elapsedNs, alpha);
        while (!averageNs_.compare_exchange_weak(prevAvg, nextAvg, std::memory_order_relaxed)) {
            nextAvg = Math::EMA(prevAvg, elapsedNs, alpha);
        }

        m_running = false;
        return Duration(elapsedNs);
    }

    void Timer::ResetThread() {
        m_startNs = 0;
        m_lastNs = 0;
        m_totalNs = 0;
        m_count = 0;
    }

    void Timer::ResetGlobal() {
        totalNs_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        longestNs_.store(0, std::memory_order_relaxed);
        averageNs_.store(0, std::memory_order_relaxed);
    }

    Timer::Duration Timer::GetLastElapsed() {
        return Duration(m_lastNs);
    }

    Timer::Duration Timer::GetTotalElapsed() {
        return Duration(m_totalNs);
    }

    Timer::Duration Timer::GetTotalGlobalElapsed()
    {
        return Duration(totalNs_.load(std::memory_order_relaxed));
    }

    Timer::Duration Timer::GetLongestElapsed() {
        return Duration(longestNs_.load(std::memory_order_relaxed));
    }

    Timer::Duration Timer::GetEMAAverageElapsed() {
        return Duration(static_cast<long long>(averageNs_.load(std::memory_order_relaxed)));
    }

    Timer::Duration Timer::GetArithmeticAverageElapsed() {
        if (m_count <= 0) return Duration{ 0 };
        return m_count == 0 ? Duration(0) : Duration(m_totalNs / m_count);
    }

    Timer::Duration Timer::GetArithmeticAverageGlobalElapsed() {
        long long sum = count_.load(std::memory_order_relaxed);
        if (sum <= 0) return Duration{ 0 };
        return sum == 0 ? Duration(0) : Duration(totalNs_.load(std::memory_order_relaxed) / sum);
    }

    bool Timer::IsRunning() {
        return m_running;
    }

    std::string Timer::ToString(Duration duration) {
        using namespace std::chrono;
        auto ns = duration_cast<nanoseconds>(duration).count();
        auto h = ns / 3'600'000'000'000LL; ns %= 3'600'000'000'000LL;
        auto m = ns / 60'000'000'000LL; ns %= 60'000'000'000LL;
        auto s = ns / 1'000'000'000LL; ns %= 1'000'000'000LL;
        auto ms = ns / 1'000'000LL; ns %= 1'000'000LL;
        auto us = ns / 1'000LL; ns %= 1'000LL;

        char buffer[128];
        snprintf(buffer, sizeof(buffer), "%02lluh %02llum %02llus %03llums %03lluus %lluns",
            h, m, s, ms, us, ns);
        return std::string(buffer);
    }

    int64_t Timer::NowNs()
    {
#if defined(_WIN32)
        static LARGE_INTEGER frequency = [] {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            return freq;
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<int64_t>(counter.QuadPart) * 1'000'000'000LL / frequency.QuadPart;
#else
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + static_cast<int64_t>(ts.tv_nsec);
#endif
    }
}
