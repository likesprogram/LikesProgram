#include "../../include/LikesProgram/ThreadPool.hpp"
#include "../../include/LikesProgram/Timer.hpp"
#include "../../include/LikesProgram/CoreUtils.hpp"
#include "../../include/LikesProgram/math/Math.hpp"
#include <sstream>
#include <iomanip>

namespace LikesProgram {
    struct ThreadPool::ThreadPoolImpl {
        Options opts_;

        // 队列/同步结构
        mutable std::mutex queueMutex_;
        std::condition_variable queueNotEmptyCv_; // 通知 worker
        std::condition_variable queueNotFullCv_;  // 通知 submit 等待
        std::deque<std::function<void()>> taskQueue_; // 任务队列
        size_t queueCapacity_ = 1000; // 队列容量

        // 工作线程容器（保持到最终join；活跃数用 aliveThreads_ 统计）
        std::vector<std::thread> workers_;
        mutable std::mutex workersMutex_;

        // 运行标志
        std::atomic<bool> running_{ false };       // 池是否处于运行状态（允许worker取任务）
        std::atomic<bool> acceptTasks_{ false };   // 是否接受新任务（入队）
        std::atomic<bool> shutdownNowFlag_{ false }; // 用于唤醒空闲 worker 退出（shutdown）

        // 统计
        std::atomic<size_t> submittedCount_{ 0 };  // 成功入队的任务数
        std::atomic<size_t> rejectedCount_{ 0 };   // 被拒绝的任务数
        std::atomic<size_t> completedCount_{ 0 };  // 完成任务数
        std::atomic<size_t> activeCount_{ 0 };     // 正在执行的任务数
        std::atomic<size_t> aliveThreads_{ 0 };    // 存活线程数
        std::atomic<size_t> largestPoolSize_{ 0 }; // 历史最大线程数
        std::atomic<size_t> peakQueueSize_{ 0 };   // 队列峰值
        Timer timer_;

        // 时间点（纳秒）
        std::atomic<long long> lastSubmitNs_{ 0 };   // steady_clock::now().time_since_epoch() 的纳秒数
        std::atomic<long long> lastFinishNs_{ 0 };

        // 线程退出等待
        mutable std::mutex workerExitMutex_;
        std::condition_variable workerExitCv_;
    };

    String ThreadPool::Statistics::ToString() const {
        LikesProgram::String statsStr;
        statsStr.Append(u"成功入队的任务数：")
            .Append(LikesProgram::String(std::to_string(submitted))).Append(u"\r\n");
        statsStr.Append(u"被拒绝的任务数：")
            .Append(LikesProgram::String(std::to_string(rejected))).Append(u"\r\n");
        statsStr.Append(u"执行完成的任务数：")
            .Append(LikesProgram::String(std::to_string(completed))).Append(u"\r\n");
        statsStr.Append(u"正在执行的任务数：")
            .Append(LikesProgram::String(std::to_string(active))).Append(u"\r\n");
        statsStr.Append(u"存活工作线程数：")
            .Append(LikesProgram::String(std::to_string(aliveThreads))).Append(u"\r\n");
        statsStr.Append(u"历史最大线程数：")
            .Append(LikesProgram::String(std::to_string(largestPoolSize))).Append(u"\r\n");
        statsStr.Append(u"队列峰值：")
            .Append(LikesProgram::String(std::to_string(peakQueueSize))).Append(u"\r\n");

        if (lastSubmitTime.time_since_epoch().count() != 0) {
            auto tt = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lastSubmitTime - std::chrono::steady_clock::now()
                    + std::chrono::system_clock::now()));
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::wostringstream oss;
            oss << std::put_time(&tm, L"%F %T");
            statsStr.Append(u"最后一次提交时间：")
                .Append(LikesProgram::String(oss.str())).Append(u"\r\n");
        }

        if (lastFinishTime.time_since_epoch().count() != 0) {
            auto tt = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lastFinishTime - std::chrono::steady_clock::now()
                    + std::chrono::system_clock::now()));
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::wostringstream oss;
            oss << std::put_time(&tm, L"%F %T");
            statsStr.Append(u"最后一次完成时间：")
                .Append(LikesProgram::String(oss.str())).Append(u"\r\n");
        }

        statsStr.Append(u"最长任务耗时：")
            .Append(LikesProgram::Timer::ToString(longestTaskTime)).Append(u"\r\n");
        statsStr.Append(u"算术平均任务耗时：")
            .Append(LikesProgram::Timer::ToString(arithmeticAverageTaskTime)).Append(u"\r\n");
        statsStr.Append(u"指数移动平均任务耗时：")
            .Append(LikesProgram::Timer::ToString(averageTaskTime)).Append(u"\r\n");

        return statsStr;
    }

    ThreadPool::ThreadPool(Options opts): m_impl(new ThreadPoolImpl) {
        m_impl->opts_ = std::move(opts);
        m_impl->queueCapacity_ = opts.queueCapacity;
        m_impl->timer_ = Timer();
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
                m_impl->rejectedCount_.fetch_add(m_impl->taskQueue_.size(), std::memory_order_relaxed);
                m_impl->taskQueue_.clear();
            }
            break;
        }

        NotifyAllWorkers();
        m_impl->queueNotFullCv_.notify_all(); // 保证等待入队的线程也退出
    }

    bool ThreadPool::AwaitTermination(std::chrono::milliseconds timeout) {
        if (timeout.count() == 0) {
            return m_impl->aliveThreads_.load(std::memory_order_acquire) == 0;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lk(m_impl->workerExitMutex_);
        return m_impl->workerExitCv_.wait_until(lk, deadline, [&] { return m_impl->aliveThreads_.load(std::memory_order_acquire) == 0; });
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
        return m_impl->activeCount_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::GetThreadCount() const {
        return m_impl->aliveThreads_.load(std::memory_order_acquire);
    }

    bool ThreadPool::IsRunning() const {
        return m_impl->running_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetRejectedCount() const {
        return m_impl->rejectedCount_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetTotalTasksSubmitted() const {
        return m_impl->submittedCount_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetCompletedCount() const {
        return m_impl->completedCount_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetLargestPoolSize() const {
        return m_impl->largestPoolSize_.load(std::memory_order_acquire);
    }

    size_t ThreadPool::IetPeakQueueSize() const {
        return m_impl->peakQueueSize_.load(std::memory_order_acquire);
    }

    ThreadPool::Statistics ThreadPool::Snapshot() const {
        Statistics s;
        s.submitted = m_impl->submittedCount_.load();
        s.rejected = m_impl->rejectedCount_.load();
        s.completed = m_impl->completedCount_.load();
        s.active = m_impl->activeCount_.load();
        s.aliveThreads = m_impl->aliveThreads_.load();
        s.largestPoolSize = m_impl->largestPoolSize_.load();
        s.peakQueueSize = m_impl->peakQueueSize_.load();

        long long lastSubmitNs = m_impl->lastSubmitNs_.load();
        long long lastFinishNs = m_impl->lastFinishNs_.load();
        if (lastSubmitNs > 0)
            s.lastSubmitTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastSubmitNs));
        if (lastFinishNs > 0)
            s.lastFinishTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastFinishNs));

        s.longestTaskTime = m_impl->timer_.GetLongestElapsed();
        s.averageTaskTime = m_impl->timer_.GetEMAAverageElapsed();
        s.arithmeticAverageTaskTime = m_impl->timer_.GetArithmeticAverageElapsed();
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
            m_impl->rejectedCount_.fetch_add(1, std::memory_order_relaxed);
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
                    m_impl->rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                break;
            case RejectPolicy::Discard:
                m_impl->rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                return false;
            case RejectPolicy::DiscardOld:
                if (!m_impl->taskQueue_.empty()) m_impl->taskQueue_.pop_front();
                break;
            case RejectPolicy::Throw:
                throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
            }
        }

        m_impl->taskQueue_.emplace_back(std::move(task));
        m_impl->submittedCount_.fetch_add(1, std::memory_order_relaxed);

        // 记录最近一次提交时间（统一使用纳秒）
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        m_impl->lastSubmitNs_.store(now_ns, std::memory_order_relaxed);

        // 峰值队列
        const size_t qsz = m_impl->taskQueue_.size();
        Math::UpdateMax(m_impl->peakQueueSize_, qsz);

        m_impl->queueNotEmptyCv_.notify_one();

        // 动态扩容：队列长度 > 活跃线程数 且还没到 maxThreads
        if (m_impl->opts_.allowDynamicResize && m_impl->running_.load(std::memory_order_acquire)) {
            size_t alive = m_impl->aliveThreads_.load(std::memory_order_acquire);
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

        // 更新历史最大线程数（spawnWorker 已把 aliveThreads_++）
        {
            size_t cur = m_impl->aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(m_impl->largestPoolSize_, cur);
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
                if (m_impl->taskQueue_.empty() && m_impl->opts_.allowDynamicResize && m_impl->aliveThreads_.load(std::memory_order_acquire) > m_impl->opts_.coreThreads) {
                    break;
                }

                if (!m_impl->taskQueue_.empty()) {
                    task = std::move(m_impl->taskQueue_.front());
                    m_impl->taskQueue_.pop_front();
                    m_impl->queueNotFullCv_.notify_one();
                }
            }

            if (task) {
                m_impl->activeCount_.fetch_add(1, std::memory_order_relaxed);
                Timer timer(true, &m_impl->timer_);
                try {
                    task();
                }
                catch (...) {
                    if (m_impl->opts_.exceptionHandler) {
                        try { m_impl->opts_.exceptionHandler(std::current_exception()); }
                        catch (...) {} // 回调自己异常也吞掉，避免杀死worker
                    }
                }
                auto elapsed = timer.Stop(); // 自动更新全局 EMA 和最长耗时
                m_impl->completedCount_.fetch_add(1, std::memory_order_relaxed);
                m_impl->activeCount_.fetch_sub(1, std::memory_order_relaxed);
                m_impl->lastFinishNs_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count(), std::memory_order_relaxed
                );
            }

            if (tryExit()) break;
        }

        // 线程退出
        {
            std::lock_guard<std::mutex> lk(m_impl->workerExitMutex_);
            long long prev = m_impl->aliveThreads_.load(std::memory_order_acquire);
            m_impl->aliveThreads_.store((prev > 0 ? prev - 1 : 0), std::memory_order_release);
            if (m_impl->aliveThreads_.load(std::memory_order_acquire) == 0) m_impl->workerExitCv_.notify_all();
        }
    }

    void ThreadPool::SpawnWorker() {
        {
            std::lock_guard<std::mutex> lk(m_impl->workerExitMutex_);
            m_impl->aliveThreads_.fetch_add(1, std::memory_order_relaxed);
            size_t cur = m_impl->aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(m_impl->largestPoolSize_, cur);
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
}