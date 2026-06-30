#include <LikesProgram/Core/time/Timer.hpp>

#include <mutex>
#include <shared_mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace LikesProgram {
    namespace Time {
        struct Timer::TimerImpl {
            std::atomic<uint64_t> m_startNs = 0;       // 当前计时起点，单位 ns
            std::atomic<uint64_t> m_lastNs = 0;        // 最近一次 Stop 的耗时，单位 ns
            std::atomic<uint64_t> m_accumulatedNs = 0; // 自身与子计时器累计耗时，单位 ns
            std::atomic<bool> m_running = false;       // 当前是否处于计时状态
            mutable std::shared_mutex m_mutex;         // 保护析构/重置与 Stop 的并发关系
        };

        Timer::Timer(bool autoStart, Timer* parent) : m_impl(new TimerImpl{}), m_parent(parent) {
            if (autoStart) Start();
        }

        void Timer::SetParent(Timer* parent) {
            m_parent = parent;
        }

        Timer::~Timer() {
            if (!m_impl) return;
            TimerImpl* impl = m_impl; // 待释放的实现对象
            m_impl = nullptr;
            std::unique_lock lk(impl->m_mutex); // 等待并发 Stop/Reset 离开临界区
            lk.unlock();
            delete impl;
        }

        Timer::Timer(const Timer& other) : m_parent(other.m_parent) {
            m_impl = new TimerImpl{};
            if (other.m_impl) {
                m_impl->m_startNs.store(other.m_impl->m_startNs.load());
                m_impl->m_lastNs.store(other.m_impl->m_lastNs.load());
                m_impl->m_accumulatedNs.store(other.m_impl->m_accumulatedNs.load());
                m_impl->m_running.store(false);
            }
        }

        Timer& Timer::operator=(const Timer& other) {
            if (this != &other) {
                TimerImpl* newImpl = new TimerImpl{}; // 先构造新状态，保证赋值异常安全
                if (other.m_impl) {
                    newImpl->m_startNs.store(other.m_impl->m_startNs.load());
                    newImpl->m_lastNs.store(other.m_impl->m_lastNs.load());
                    newImpl->m_accumulatedNs.store(other.m_impl->m_accumulatedNs.load());
                    newImpl->m_running.store(false);
                }
                delete m_impl;
                m_impl = newImpl;
                m_parent = other.m_parent;
            }
            return *this;
        }

        Timer::Timer(Timer&& other) noexcept : m_impl(other.m_impl), m_parent(other.m_parent) {
            other.m_impl = nullptr;
            // 移动后源计时器不再关联父计时器，避免重复累计。
            // 目标对象接管全部计时状态，源对象仅保持可析构。
            other.m_parent = nullptr;
        }

        Timer& Timer::operator=(Timer&& other) noexcept {
            if (this != &other) {
                // 移动赋值直接转移实现和父计时器指针。
                delete m_impl;
                m_impl = other.m_impl;
                m_parent = other.m_parent;
                // 源对象清空指针，避免析构时重复释放或重复累计。
                other.m_impl = nullptr;
                other.m_parent = nullptr;
            }
            return *this;
        }

        void Timer::Start() {
            if (!m_impl) return;
            m_impl->m_startNs.store(NowNs(), std::memory_order_relaxed);
            m_impl->m_running.store(true, std::memory_order_relaxed);
        }

        Time::Duration Timer::Stop() {
            if (!m_impl) return Duration(0);
            std::shared_lock lk(m_impl->m_mutex); // 允许并发读取，阻止析构释放状态
            if (!m_impl->m_running.load(std::memory_order_relaxed)) return Duration(0);

            uint64_t elapsedNs = NowNs() - m_impl->m_startNs.load(std::memory_order_relaxed); // 本次耗时 ns
            m_impl->m_lastNs.store(elapsedNs, std::memory_order_relaxed);
            m_impl->m_accumulatedNs.fetch_add(elapsedNs, std::memory_order_relaxed);

            if (m_parent && m_parent->m_impl && m_parent->m_impl != m_impl) {
                m_parent->m_impl->m_accumulatedNs.fetch_add(elapsedNs, std::memory_order_relaxed);
            }

            // Stop 成功后置为非运行，重复 Stop 返回 0。
            // Duration 使用 chrono 类型返回，单位保持纳秒。
            m_impl->m_running.store(false, std::memory_order_relaxed);
            return Duration(elapsedNs);
        }

        void Timer::Reset() {
            if (!m_impl) return;
            std::unique_lock lk(m_impl->m_mutex); // 重置所有计时状态需要独占
            m_impl->m_startNs.store(0, std::memory_order_relaxed);
            m_impl->m_lastNs.store(0, std::memory_order_relaxed);
            // Reset 清零累计值并终止运行状态。
            m_impl->m_accumulatedNs.store(0, std::memory_order_relaxed);
            m_impl->m_running.store(false, std::memory_order_relaxed);
        }

        Time::Duration Timer::GetLastElapsed() const {
            if (!m_impl) return Duration(0);
            return Duration(m_impl->m_lastNs.load(std::memory_order_relaxed));
        }

        Time::Duration Timer::GetAccumulatedElapsed() const {
            if (!m_impl) return Duration(0);
            return Duration(m_impl->m_accumulatedNs.load(std::memory_order_relaxed));
        }

        bool Timer::IsRunning() const {
            if (!m_impl) return false;
            return m_impl->m_running.load(std::memory_order_relaxed);
        }

        uint64_t Timer::NowNs()
        {
#if defined(_WIN32)
            static LARGE_INTEGER frequency = [] { // 每进程缓存的高精度计时器频率
                LARGE_INTEGER freq; // QueryPerformanceCounter 每秒 tick 数
                QueryPerformanceFrequency(&freq);
                return freq;
                }();
            LARGE_INTEGER counter; // 当前高精度 tick 值
            QueryPerformanceCounter(&counter);
            return static_cast<uint64_t>(counter.QuadPart) * 1'000'000'000ULL / frequency.QuadPart;
#else
            timespec ts; // CLOCK_MONOTONIC_RAW 当前时间
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }
    }
}
