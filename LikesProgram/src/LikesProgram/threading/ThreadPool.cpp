#include "../../../include/LikesProgram/threading/ThreadPool.hpp"
#include "../../../include/LikesProgram/time/Timer.hpp"
#include "../../../include/LikesProgram/time/Time.hpp"
#include "../../../include/LikesProgram/system/CoreUtils.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"
#include "../../../include/LikesProgram/metrics/Counter.hpp"
#include "../../../include/LikesProgram/metrics/Gauge.hpp"
#include "../../../include/LikesProgram/threading/IThreadPoolObserver.hpp"
#include <sstream>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <tuple>
#include <utility>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stringapiset.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <pthread.h>
#endif

namespace LikesProgram {
    struct ThreadPool::ThreadPoolImpl {
        Options opts_;

        // 队列/同步结构
        mutable std::mutex queueMutex_;
        std::condition_variable queueNotEmptyCv_; // 通知 worker
        std::condition_variable queueNotFullCv_;  // 通知 submit 等待
        std::deque<std::function<void()>> taskQueue_; // 任务队列
        size_t queueCapacity_ = 1000; // 队列容量

        // 工作线程容器（保持到最终join；活跃数用 m_aliveThreads 统计）
        std::vector<std::thread> workers_;
        mutable std::mutex workersMutex_;

        // 运行标志
        std::atomic<bool> running_{ false };       // 池是否处于运行状态（允许worker取任务）
        std::atomic<bool> acceptTasks_{ false };   // 是否接受新任务（入队）
        std::atomic<bool> shutdownNowFlag_{ false }; // 用于唤醒空闲 worker 退出（shutdown）

        Time::Timer timer_;

        // 统计
        std::atomic<size_t> m_submittedCount{ 0 };  // 成功入队的任务数
        std::atomic<size_t> m_rejectedCount{ 0 };   // 被拒绝的任务数
        std::atomic<size_t> m_completedCount{ 0 };  // 完成任务数
        std::atomic<size_t> m_queueSize{ 0 };
        std::atomic<size_t> m_peakQueueSize{ 0 };   // 队列峰值

        std::atomic<size_t> m_activeTasks{ 0 };     // 正在执行的任务数
        std::atomic<size_t> m_aliveThreads{ 0 };    // 存活线程数
        std::atomic<size_t> m_largestPoolSize{ 0 }; // 历史最大线程数
        std::atomic<long long> m_lastSubmitNs{ 0 };
        std::atomic<long long> m_lastFinishNs{ 0 };

        std::shared_ptr<IThreadPoolObserver> m_observer = nullptr;


        // 线程退出等待
        mutable std::mutex workerExitMutex_;
        std::condition_variable workerExitCv_;
    };

    String ThreadPool::Statistics::ToString() const {
        String statsStr;
        statsStr.Append(u"成功入队的任务数：")
            .Append(String(std::to_string(submitted))).Append(u"\r\n");
        statsStr.Append(u"被拒绝的任务数：")
            .Append(String(std::to_string(rejected))).Append(u"\r\n");
        statsStr.Append(u"执行完成的任务数：")
            .Append(String(std::to_string(completed))).Append(u"\r\n");
        statsStr.Append(u"正在执行的任务数：")
            .Append(String(std::to_string(active))).Append(u"\r\n");
        statsStr.Append(u"存活工作线程数：")
            .Append(String(std::to_string(aliveThreads))).Append(u"\r\n");
        statsStr.Append(u"历史最大线程数：")
            .Append(String(std::to_string(largestPoolSize))).Append(u"\r\n");
        statsStr.Append(u"队列峰值：")
            .Append(String(std::to_string(peakQueueSize))).Append(u"\r\n");
        statsStr.Append(u"最后一次提交时间：")
            .Append(Time::FormatTime(lastSubmitTime, u"%Y-%m-%d %H:%M:%S.%f")).Append(u"\r\n");
        statsStr.Append(u"最后一次完成时间：")
            .Append(Time::FormatTime(lastFinishTime, u"%Y-%m-%d %H:%M:%S.%f")).Append(u"\r\n");
        return statsStr;
    }

    ThreadPool::ThreadPool(std::shared_ptr<IThreadPoolObserver> observer, Options opts): m_impl(new ThreadPoolImpl) {
        m_impl->m_observer = observer;
        m_impl->opts_ = std::move(opts);
        m_impl->queueCapacity_ = opts.queueCapacity;
        m_impl->timer_ = Time::Timer();
        m_impl->opts_.threadNamePrefix = m_impl->opts_.threadNamePrefix.SubString(0, 15 - 5);
    }

    ThreadPool::~ThreadPool() {
        try {
            Shutdown(ShutdownPolicy::CancelNow);
            NotifyAllWorkers();
            JoinAll(); // 阻塞直到所有线程 exit & join
        }
        catch (...) {
            // 析构中不抛
        }

        // 释放 m_impl
        if (m_impl) delete m_impl;
        m_impl = nullptr;
    }

    void ThreadPool::Start() {
        bool expected = false;
        if (!m_impl->running_.compare_exchange_strong(expected, true)) {
            return; // 已经启动
        }
        m_impl->acceptTasks_.store(true, std::memory_order_release);

        // 至少起 coreThreads 个
        for (size_t i = 0; i < m_impl->opts_.coreThreads; ++i) SpawnWorker();
    }

    void ThreadPool::Shutdown(ShutdownPolicy mode) {
        // 已经停止
        if (!m_impl->running_.load(std::memory_order_acquire) &&
            !m_impl->acceptTasks_.load(std::memory_order_acquire)) {
            return;
        }

        switch (mode) {
        case ShutdownPolicy::Graceful:
            // 不接受新任务；允许 worker 继续取队列直到队列与正在运行任务跑完
            m_impl->acceptTasks_.store(false, std::memory_order_release);
            m_impl->running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::Drain:
            // 语义：拒绝新任务，但允许队列中已有任务被执行。不要清空队列。
            m_impl->acceptTasks_.store(false, std::memory_order_release);
            m_impl->running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::CancelNow:
            m_impl->acceptTasks_.store(false, std::memory_order_release);
            m_impl->running_.store(false, std::memory_order_release);
            m_impl->shutdownNowFlag_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(m_impl->queueMutex_);
                m_impl->m_rejectedCount.fetch_add(m_impl->taskQueue_.size(), std::memory_order_relaxed);
                m_impl->m_observer->OnTaskRejected();
                m_impl->taskQueue_.clear();
            }
            break;
        }

        NotifyAllWorkers();
        m_impl->queueNotFullCv_.notify_all(); // 保证等待入队的线程也退出
    }

    bool ThreadPool::AwaitTermination(std::chrono::milliseconds timeout) {
        if (timeout.count() == 0) {
            return m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lk(m_impl->workerExitMutex_);
        return m_impl->workerExitCv_.wait_until(lk, deadline, [&] { return m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0; });
    }

    bool ThreadPool::PostNoArg(std::function<void()> fn) {
        bool success = EnqueueTask(std::move(fn));
        if (!success && m_impl->opts_.exceptionHandler) m_impl->opts_.exceptionHandler(std::make_exception_ptr(std::runtime_error("Task rejected")));
        return success;
    }

    size_t ThreadPool::GetQueueSize() const {
        std::lock_guard<std::mutex> lk(m_impl->queueMutex_);
        return m_impl->taskQueue_.size();
    }

    size_t ThreadPool::GetActiveCount() const {
        return m_impl->m_activeTasks.load(std::memory_order_acquire);
    }

    size_t ThreadPool::GetThreadCount() const {
        return m_impl->m_aliveThreads.load(std::memory_order_acquire);
    }

    bool ThreadPool::IsRunning() const {
        return m_impl->running_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetRejectedCount() const {
        return m_impl->m_rejectedCount.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetTotalTasksSubmitted() const {
        return m_impl->m_submittedCount.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetCompletedCount() const {
        return m_impl->m_completedCount.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetLargestPoolSize() const {
        return m_impl->m_largestPoolSize.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetPeakQueueSize() const {
        return m_impl->m_peakQueueSize.load(std::memory_order_acquire);
    }

    ThreadPool::Statistics ThreadPool::Snapshot() const {
        Statistics s;
        s.submitted = m_impl->m_submittedCount.load();
        s.rejected = m_impl->m_rejectedCount.load();
        s.completed = m_impl->m_completedCount.load();
        s.active = m_impl->m_activeTasks.load();
        s.aliveThreads = m_impl->m_aliveThreads.load();
        s.largestPoolSize = m_impl->m_largestPoolSize.load();
        s.peakQueueSize = m_impl->m_peakQueueSize.load();

        long long lastSubmitNs = m_impl->m_lastSubmitNs.load();
        long long lastFinishNs = m_impl->m_lastFinishNs.load();
        if (lastSubmitNs > 0)
            s.lastSubmitTime = Time::NsToSystemClock(lastSubmitNs);
        if (lastFinishNs > 0)
            s.lastFinishTime = Time::NsToSystemClock(lastFinishNs);
        return s;
    }

    void ThreadPool::JoinAll() {
        std::lock_guard<std::mutex> lk(m_impl->workersMutex_);
        for (auto& t : m_impl->workers_) {
            if (t.joinable()) t.join();
        }
        m_impl->workers_.clear();
    }

    bool ThreadPool::EnqueueTask(std::function<void()>&& task) {
        if (!m_impl->running_.load(std::memory_order_acquire) || !m_impl->acceptTasks_.load(std::memory_order_acquire)) {
            m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
            m_impl->m_observer->OnTaskRejected();
            return false;
        }

        std::unique_lock<std::mutex> lock(m_impl->queueMutex_);

        if (m_impl->taskQueue_.size() >= m_impl->queueCapacity_) {
            switch (m_impl->opts_.rejectPolicy) {
            case RejectPolicy::Block:
                m_impl->queueNotFullCv_.wait(lock, [&] {
                    return m_impl->taskQueue_.size() < m_impl->queueCapacity_ || !m_impl->acceptTasks_.load(std::memory_order_acquire) || m_impl->shutdownNowFlag_.load(std::memory_order_acquire);
                    });
                if (!m_impl->running_.load(std::memory_order_acquire) || !m_impl->acceptTasks_.load(std::memory_order_acquire) || m_impl->shutdownNowFlag_.load(std::memory_order_acquire)) {
                    m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                    m_impl->m_observer->OnTaskRejected();
                    return false;
                }
                break;
            case RejectPolicy::Discard:
                m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_observer->OnTaskRejected();
                return false;
            case RejectPolicy::DiscardOld:
                if (!m_impl->taskQueue_.empty()) m_impl->taskQueue_.pop_front();
                break;
            case RejectPolicy::Throw:
                throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
            }
        }

        m_impl->taskQueue_.emplace_back(std::move(task));
        m_impl->m_submittedCount.fetch_add(1, std::memory_order_relaxed);

        // 记录最近一次提交时间（统一使用纳秒）
        auto now_ns = std::chrono::duration_cast<Time::Nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m_impl->m_lastSubmitNs.store(now_ns, std::memory_order_relaxed);

        // 峰值队列
        const size_t qsz = m_impl->taskQueue_.size();
        Math::UpdateMax(m_impl->m_peakQueueSize, qsz);

        // 更新统计信息
        if(m_impl->m_observer) m_impl->m_observer->OnTaskSubmitted((double)qsz);

        m_impl->queueNotEmptyCv_.notify_one();

        // 动态扩容：队列长度 > 活跃线程数 且还没到 maxThreads
        if (m_impl->opts_.allowDynamicResize && m_impl->running_.load(std::memory_order_acquire)) {
            size_t alive = m_impl->m_aliveThreads.load(std::memory_order_acquire);
            if (alive < m_impl->opts_.maxThreads && qsz > alive) {
                SpawnWorker();
            }
        }
        return true;
    }

    void ThreadPool::WorkerLoop() {
        // 线程命名（可选）
        if (!m_impl->opts_.threadNamePrefix.Empty()) {
            std::wostringstream woss;
            // 获取当前线程的 ID 并直接转换为整数（通常是一个指针值）
            std::thread::id threadId = std::this_thread::get_id();

            // 将 std::thread::id 转换为整数值（指针值），直接作为唯一标识符
            std::uintptr_t idValue = reinterpret_cast<std::uintptr_t>(&threadId);

            // 保证生成一个 5 位数 ID（通过对 idValue 进行模运算）
            size_t threadIdNum = idValue % 100000;  // 保证为 5 位数

            // 确保输出为 5 位数（不足前面补零）
            woss << std::setw(5) << std::setfill(L'0') << threadIdNum;

            // 创建线程名称
            String threadName;
            threadName.Append(m_impl->opts_.threadNamePrefix).Append(String(woss.str()));
            CoreUtils::SetCurrentThreadName(threadName);
        }

        auto tryExit = [this]() -> bool {
            if (m_impl->shutdownNowFlag_.load(std::memory_order_acquire)) return true;
            if (!m_impl->running_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(m_impl->queueMutex_);
                return m_impl->taskQueue_.empty(); // drain 完队列即可退出
            }
            return false;
        };

        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(m_impl->queueMutex_);

                if (!m_impl->shutdownNowFlag_.load(std::memory_order_acquire)) {
                    // 空闲等待，带 keepAlive，用于缩容
                    m_impl->queueNotEmptyCv_.wait_for(lock, m_impl->opts_.keepAlive, [&] {
                        return !m_impl->taskQueue_.empty() || m_impl->shutdownNowFlag_.load(std::memory_order_acquire) || !m_impl->running_.load(std::memory_order_acquire);
                    });
                }

                // CancelNow 直接退
                if (m_impl->shutdownNowFlag_.load(std::memory_order_acquire)) break;

                // Graceful/Drain：队列空且不再运行 -> 退出
                if (!m_impl->running_.load(std::memory_order_acquire) && m_impl->taskQueue_.empty()) break;

                // 动态缩容：超时空闲且线程数超过 core -> 退出
                if (m_impl->taskQueue_.empty() && m_impl->opts_.allowDynamicResize && m_impl->m_aliveThreads.load(std::memory_order_acquire) > m_impl->opts_.coreThreads) {
                    break;
                }

                // 获取任务
                if (!m_impl->taskQueue_.empty()) {
                    task = std::move(m_impl->taskQueue_.front());
                    m_impl->taskQueue_.pop_front();
                    m_impl->queueNotFullCv_.notify_one();
                }
            }

            if (task) {
                m_impl->m_activeTasks.fetch_add(1, std::memory_order_relaxed);
                // 更新统计信息
                if (m_impl->m_observer) m_impl->m_observer->OnTaskStarted();
                Time::Timer timer(true, &m_impl->timer_);
                try {
                    task();
                }
                catch (...) {
                    if (m_impl->opts_.exceptionHandler) {
                        try { m_impl->opts_.exceptionHandler(std::current_exception()); }
                        catch (...) {} // 回调自己异常也吞掉，避免杀死worker
                    }
                }

                m_impl->m_completedCount.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_activeTasks.fetch_sub(1, std::memory_order_relaxed);
                auto now_ns = std::chrono::duration_cast<Time::Nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                m_impl->m_lastFinishNs.store(now_ns, std::memory_order_relaxed);
                // 更新统计信息
                if (m_impl->m_observer) m_impl->m_observer->OnTaskCompleted(timer.Stop(), (double)m_impl->taskQueue_.size());
            }

            if (tryExit()) break;
        }

        // 线程退出
        {
            std::lock_guard<std::mutex> lk(m_impl->workerExitMutex_);
            auto prev = m_impl->m_aliveThreads.load(std::memory_order_acquire);
            m_impl->m_aliveThreads.store((prev > 0 ? prev - 1 : 0), std::memory_order_release);
            // 更新统计信息
            if (m_impl->m_observer) m_impl->m_observer->OnThreadCountRemoved();
            if (m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0) m_impl->workerExitCv_.notify_all();
        }
    }

    void ThreadPool::SpawnWorker() {
        {
            std::lock_guard<std::mutex> lk(m_impl->workerExitMutex_);
            auto cur = m_impl->m_aliveThreads.fetch_add(1, std::memory_order_relaxed) + 1;
            // 更新最大线程数
            Math::UpdateMax(m_impl->m_largestPoolSize, cur);
            // 更新统计信息
            if (m_impl->m_observer) m_impl->m_observer->OnThreadCountAdded();
        }

        std::thread t([this] { WorkerLoop(); });
        {
            std::lock_guard<std::mutex> lk(m_impl->workersMutex_);
            m_impl->workers_.emplace_back(std::move(t));
        }
    }

    void ThreadPool::NotifyAllWorkers() {
        m_impl->queueNotEmptyCv_.notify_all();
        m_impl->queueNotFullCv_.notify_all();
    }
    std::function<void(std::exception_ptr)> ThreadPool::GetExceptionHandler() const {
        return m_impl->opts_.exceptionHandler;
    }

    std::shared_ptr<IThreadPoolObserver> ThreadPool::CreateDefaultThreadPoolMetrics(const String& poolName, std::shared_ptr<Metrics::Registry> registry)
    {
        return std::make_shared<ThreadPoolObserverBase>(poolName, registry);
    }
}