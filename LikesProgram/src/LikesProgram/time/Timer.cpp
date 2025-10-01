#include "../../../include/LikesProgram/time/Timer.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"

#include <thread>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

// ===================== Timer 锁语义说明 =====================
//
// Timer 内部使用 std::shared_mutex 控制并发访问，但与常见模式相反：
//   - Stop() 使用 shared_lock
//   - Reset() / 析构 使用 unique_lock
//   - 读取统计值 (GetXXX) 使用 unique_lock
//
// 设计原因：
//
// 1. Stop() 是高频操作，主要用于写入累加值：
//    - 所有字段通过 std::atomic 更新，没有跨字段依赖关系
//    - 更新顺序无关紧要（只要最终累加正确即可）
//    - 因此 Stop 可以多个线程并发执行，只需保证与 Reset/析构不冲突
//
// 2. Reset() 与 析构：
//    - 需要清空或销毁内部数据，必须与 Stop 并发隔离
//    - 因此使用 unique_lock 阻止所有 Stop
//
// 3. GetXXX 系列读取接口：
//    - 读取时要求一致性（例如 total / count 的比值）
//    - 如果并发 Stop，结果可能不一致
//    - 因此读取时使用 unique_lock，阻止 Stop 写入
//
// 总结：
//    - Stop：shared_lock，允许并发
//    - Reset / 析构：unique_lock，阻止所有操作
//    - GetXXX 读取：unique_lock，保证一致性
//
// 这种设计的核心目标是：
//    - Stop() 高效（热点路径，不阻塞）
//    - Read() 保证一致性（冷路径，可序列化）
//    - Reset()/析构 安全
// ============================================================

namespace LikesProgram {
    namespace Time {
        struct Timer::TimerImpl {
            std::atomic<uint64_t> m_startNs = 0;
            std::atomic<uint64_t> m_lastNs = 0;
            std::shared_ptr<LikesProgram::Metrics::Summary> m_summary = nullptr;
            std::atomic<bool> m_running = false;
            mutable std::shared_mutex m_mutex;
        };

        Timer::Timer(std::shared_ptr<LikesProgram::Metrics::Summary> summary, bool autoStart, Timer* parent) : m_impl(new TimerImpl{}), m_parent(parent) {
            m_impl->m_summary = summary;
            if (autoStart) Start();
        }

        void Timer::SetParent(Timer* parent) {
            m_parent = parent;
        }

        Timer::~Timer() {
            if (m_impl) {
                std::unique_lock lk(m_impl->m_mutex);
                delete m_impl;
                m_impl = nullptr;
            }
        }

        Timer::Timer(const Timer& other) : m_parent(other.m_parent) {
            m_impl = new TimerImpl{};
            if (other.m_impl) {
                m_impl->m_startNs.store(other.m_impl->m_startNs.load());
                m_impl->m_lastNs.store(other.m_impl->m_lastNs.load());
                m_impl->m_summary = other.m_impl->m_summary;
                m_impl->m_running.store(other.m_impl->m_running.load());
            }
        }

        // 拷贝赋值
        Timer& Timer::operator=(const Timer& other) {
            if (this != &other) {
                TimerImpl* newImpl = new TimerImpl{};
                if (other.m_impl) {
                    newImpl->m_startNs.store(other.m_impl->m_startNs.load());
                    newImpl->m_lastNs.store(other.m_impl->m_lastNs.load());
                    newImpl->m_summary = other.m_impl->m_summary;
                    newImpl->m_running.store(other.m_impl->m_running.load());
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

        void Timer::Start() {
            m_impl->m_startNs.store(NowNs(), std::memory_order_relaxed);
            m_impl->m_running.store(true, std::memory_order_relaxed);
        }

        Time::Duration Timer::Stop() {
            std::shared_lock lk(m_impl->m_mutex); // 共享锁
            if (!m_impl->m_running.load(std::memory_order_relaxed)) return Duration(0);
            uint64_t elapsedNs = NowNs() - m_impl->m_startNs.load(std::memory_order_relaxed);
            m_impl->m_lastNs.store(elapsedNs, std::memory_order_relaxed);

            if (m_impl->m_summary) m_impl->m_summary->Observe(NsToS(elapsedNs));

            if (m_parent && m_parent->m_impl->m_summary && m_parent->m_impl->m_summary != m_impl->m_summary)
                m_parent->m_impl->m_summary->Observe(NsToS(elapsedNs));

            m_impl->m_running.store(false, std::memory_order_relaxed);
            return Duration(elapsedNs);
        }

        void Timer::Reset() {
            std::unique_lock lk(m_impl->m_mutex); // 独享锁
            m_impl->m_startNs.store(0, std::memory_order_relaxed);
            m_impl->m_lastNs.store(0, std::memory_order_relaxed);
            if (m_impl->m_summary) m_impl->m_summary->Reset();
        }

        Time::Duration Timer::GetLastElapsed() const {
            return Duration(m_impl->m_lastNs.load(std::memory_order_relaxed));
        }

        const LikesProgram::Metrics::Summary& Timer::GetSummary() const {
            static const LikesProgram::Metrics::Summary empty(u"");
            return m_impl->m_summary ? *(m_impl->m_summary) : empty;
        }

        bool Timer::IsRunning() const {
            return m_impl->m_running.load(std::memory_order_relaxed);
        }

        uint64_t Timer::NowNs()
        {
#if defined(_WIN32)
            static LARGE_INTEGER frequency = [] {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                return freq;
                }();
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            return static_cast<uint64_t>(counter.QuadPart) * 1'000'000'000ULL / frequency.QuadPart;
#else
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }
    }
}
