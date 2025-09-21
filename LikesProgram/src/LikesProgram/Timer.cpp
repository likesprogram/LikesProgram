#include "../../include/LikesProgram/Timer.hpp"
#include "../../include/LikesProgram/math/Math.hpp"

#include <thread>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace LikesProgram {
    struct Timer::TimerImpl {
        std::atomic<int64_t> m_startNs = 0;
        std::atomic<int64_t> m_lastNs = 0;
        std::atomic<bool> m_running = false;
        std::atomic<long long> m_totalNs{ 0 };
        std::atomic<long long> m_count{ 0 };
        std::atomic<long long> m_longestNs{ 0 };
        std::atomic<double> m_averageNs{ 0 };

        mutable std::shared_mutex m_mutex;
    };

    Timer::Timer(bool autoStart, Timer* parent): m_impl(new TimerImpl()), m_parent(parent) {
        if (autoStart) Start();
    }

    void Timer::SetParent(Timer* parent) {
        m_parent = parent;
    }

    Timer::~Timer() {
        delete m_impl;
        m_impl = nullptr;
    }

    Timer::Timer(const Timer& other) : m_parent(other.m_parent) {
        m_impl = new TimerImpl;
        if (other.m_impl) {
            m_impl->m_startNs.store(other.m_impl->m_startNs.load());
            m_impl->m_lastNs.store(other.m_impl->m_lastNs.load());
            m_impl->m_running.store(other.m_impl->m_running.load());
            m_impl->m_totalNs.store(other.m_impl->m_totalNs.load());
            m_impl->m_count.store(other.m_impl->m_count.load());
            m_impl->m_longestNs.store(other.m_impl->m_longestNs.load());
            m_impl->m_averageNs.store(other.m_impl->m_averageNs.load());
            // mutex 不拷贝，每个对象自己有一把
        }
    }

    // 拷贝赋值
    Timer& Timer::operator=(const Timer& other) {
        if (this != &other) {
            TimerImpl* newImpl = new TimerImpl;
            if (other.m_impl) {
                newImpl->m_startNs.store(other.m_impl->m_startNs.load());
                newImpl->m_lastNs.store(other.m_impl->m_lastNs.load());
                newImpl->m_running.store(other.m_impl->m_running.load());
                newImpl->m_totalNs.store(other.m_impl->m_totalNs.load());
                newImpl->m_count.store(other.m_impl->m_count.load());
                newImpl->m_longestNs.store(other.m_impl->m_longestNs.load());
                newImpl->m_averageNs.store(other.m_impl->m_averageNs.load());
            }
            delete m_impl;
            m_impl = newImpl;
            m_parent = other.m_parent;
        }
        return *this;
    }

    Timer::Timer(Timer&& other) noexcept : m_impl(other.m_impl), m_parent(other.m_parent) {
        other.m_impl = nullptr;
        other.m_parent = nullptr;
    }

    Timer& Timer::operator=(Timer&& other) noexcept {
        if (this != &other) {
            delete m_impl;
            m_impl = other.m_impl;
            m_parent = other.m_parent;
            other.m_impl = nullptr;
            other.m_parent = nullptr;
        }
        return *this;
    }

    void Timer::Start(){
        m_impl->m_startNs.store(NowNs(), std::memory_order_relaxed);
        m_impl->m_running.store(true, std::memory_order_relaxed);
    }

    Timer::Duration Timer::Stop(double alpha) {
        std::shared_lock lk(m_impl->m_mutex); // 共享锁
        if (!m_impl->m_running.load(std::memory_order_relaxed)) return Duration(0);
        int64_t elapsedNs = NowNs() - m_impl->m_startNs.load(std::memory_order_relaxed);
        m_impl->m_lastNs.store(elapsedNs, std::memory_order_relaxed);

        // 累积
        m_impl->m_totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
        m_impl->m_count.fetch_add(1, std::memory_order_relaxed);


        // 更新最长耗时
        Math::UpdateMax(m_impl->m_longestNs, elapsedNs);

        // EMA 更新
        double prevAvg = m_impl->m_averageNs.load(std::memory_order_relaxed);
        double nextAvg = (prevAvg == 0.0) ? static_cast<double>(elapsedNs) : Math::EMA(prevAvg, elapsedNs, alpha);
        while (!m_impl->m_averageNs.compare_exchange_weak(prevAvg, nextAvg, std::memory_order_relaxed)) {
            nextAvg = Math::EMA(prevAvg, elapsedNs, alpha);
        }

        if (m_parent) {
            m_parent->m_impl->m_totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
            m_parent->m_impl->m_count.fetch_add(1, std::memory_order_relaxed);

            Math::UpdateMax(m_parent->m_impl->m_longestNs, elapsedNs);

            // EMA 更新
            double mPprevAvg = m_parent->m_impl->m_averageNs.load(std::memory_order_relaxed);
            double mPnextAvg = (mPprevAvg == 0.0) ? static_cast<double>(elapsedNs) : Math::EMA(mPprevAvg, elapsedNs, alpha);
            while (!m_parent->m_impl->m_averageNs.compare_exchange_weak(mPprevAvg, mPnextAvg, std::memory_order_relaxed)) {
                mPnextAvg = Math::EMA(mPprevAvg, elapsedNs, alpha);
            }
        }

        m_impl->m_running.store(false, std::memory_order_relaxed);
        return Duration(elapsedNs);
    }

    void Timer::ResetThread() {
        std::unique_lock lk(m_impl->m_mutex); // 独享锁
        m_impl->m_startNs.store(0, std::memory_order_relaxed);
        m_impl->m_lastNs.store(0, std::memory_order_relaxed);
    }

    void Timer::ResetGlobal() {
        std::unique_lock lk(m_impl->m_mutex); // 独享锁
        m_impl->m_totalNs.store(0, std::memory_order_relaxed);
        m_impl->m_count.store(0, std::memory_order_relaxed);
        m_impl->m_longestNs.store(0, std::memory_order_relaxed);
        m_impl->m_averageNs.store(0, std::memory_order_relaxed);
    }

    void Timer::Reset() {
        ResetThread();
        ResetGlobal();
    }

    Timer::Duration Timer::GetLastElapsed() const {
        return Duration(m_impl->m_lastNs.load(std::memory_order_relaxed));
    }

    Timer::Duration Timer::GetTotalElapsed() const {
        return Duration(m_impl->m_totalNs.load(std::memory_order_relaxed));
    }

    Timer::Duration Timer::GetLongestElapsed() const {
        return Duration(m_impl->m_longestNs.load(std::memory_order_relaxed));
    }

    Timer::Duration Timer::GetEMAAverageElapsed() const {
        return Duration(static_cast<long long>(m_impl->m_averageNs.load(std::memory_order_relaxed)));
    }

    Timer::Duration Timer::GetArithmeticAverageElapsed() const {
        long long sum = m_impl->m_count.load(std::memory_order_relaxed);
        if (sum <= 0) return Duration{ 0 };
        return sum == 0 ? Duration(0) : Duration(m_impl->m_totalNs.load(std::memory_order_relaxed) / sum);
    }

    bool Timer::IsRunning() const {
        return m_impl->m_running.load(std::memory_order_relaxed);
    }

    String Timer::ToString(Duration duration) {
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
        return String(buffer);
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
