#include "../../include/LikesProgram/ThreadPool.hpp"
#include "../../include/LikesProgram/Timer.hpp"
#include "../../include/LikesProgram/CoreUtils.hpp"
#include "../../include/LikesProgram/math/Math.hpp"
#include <sstream>
#include <iomanip>

namespace LikesProgram {
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

    ThreadPool::~ThreadPool() {
        try {
            Shutdown(ShutdownPolicy::CancelNow);
            NotifyAllWorkers();
            JoinAll(); // 阻塞直到所有线程 exit & join
        }
        catch (...) {
            // 析构中不抛
        }
    }

    void ThreadPool::Start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return; // 已经启动
        }
        acceptTasks_.store(true, std::memory_order_release);

        // 至少起 coreThreads 个
        for (size_t i = 0; i < opts_.coreThreads; ++i) SpawnWorker();
    }

    void ThreadPool::Shutdown(ShutdownPolicy mode) {
        // 已经停止
        if (!running_.load(std::memory_order_acquire) &&
            !acceptTasks_.load(std::memory_order_acquire)) {
            return;
        }

        switch (mode) {
        case ShutdownPolicy::Graceful:
            // 不接受新任务；允许 worker 继续取队列直到队列与正在运行任务跑完
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::Drain:
            // 语义：拒绝新任务，但允许队列中已有任务被执行。不要清空队列。
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::CancelNow:
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            shutdownNowFlag_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(queueMutex_);
                rejectedCount_.fetch_add(taskQueue_.size(), std::memory_order_relaxed);
                taskQueue_.clear();
            }
            break;
        }

        NotifyAllWorkers();
        queueNotFullCv_.notify_all(); // 保证等待入队的线程也退出
    }

    bool ThreadPool::AwaitTermination(std::chrono::milliseconds timeout) {
        if (timeout.count() == 0) {
            return aliveThreads_.load(std::memory_order_acquire) == 0;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lk(workerExitMutex_);
        return workerExitCv_.wait_until(lk, deadline, [&] { return aliveThreads_.load(std::memory_order_acquire) == 0; });
    }

    bool ThreadPool::PostNoArg(std::function<void()> fn) {
        bool success = EnqueueTask(std::move(fn));
        if (!success && opts_.exceptionHandler) opts_.exceptionHandler(std::make_exception_ptr(std::runtime_error("Task rejected")));
        return success;
    }

    size_t ThreadPool::GetQueueSize() const {
        std::lock_guard<std::mutex> lk(queueMutex_);
        return taskQueue_.size();
    }

    ThreadPool::Statistics ThreadPool::Snapshot() const {
        Statistics s;
        s.submitted = submittedCount_.load();
        s.rejected = rejectedCount_.load();
        s.completed = completedCount_.load();
        s.active = activeCount_.load();
        s.aliveThreads = aliveThreads_.load();
        s.largestPoolSize = largestPoolSize_.load();
        s.peakQueueSize = peakQueueSize_.load();

        long long lastSubmitNs = lastSubmitNs_.load();
        long long lastFinishNs = lastFinishNs_.load();
        if (lastSubmitNs > 0)
            s.lastSubmitTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastSubmitNs));
        if (lastFinishNs > 0)
            s.lastFinishTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastFinishNs));

        s.longestTaskTime = timer_.GetLongestElapsed();
        s.averageTaskTime = timer_.GetEMAAverageElapsed();
        s.arithmeticAverageTaskTime = timer_.GetArithmeticAverageElapsed();
        return s;
    }

    void ThreadPool::JoinAll() {
        std::lock_guard<std::mutex> lk(workersMutex_);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    bool ThreadPool::EnqueueTask(std::function<void()>&& task) {
        if (!running_.load(std::memory_order_acquire) || !acceptTasks_.load(std::memory_order_acquire)) {
            rejectedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::unique_lock<std::mutex> lock(queueMutex_);

        if (taskQueue_.size() >= queueCapacity_) {
            switch (opts_.rejectPolicy) {
            case RejectPolicy::Block:
                queueNotFullCv_.wait(lock, [&] {
                    return taskQueue_.size() < queueCapacity_ || !acceptTasks_.load(std::memory_order_acquire) || shutdownNowFlag_.load(std::memory_order_acquire);
                    });
                if (!running_.load(std::memory_order_acquire) || !acceptTasks_.load(std::memory_order_acquire) || shutdownNowFlag_.load(std::memory_order_acquire)) {
                    rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                break;
            case RejectPolicy::Discard:
                rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                return false;
            case RejectPolicy::DiscardOld:
                if (!taskQueue_.empty()) taskQueue_.pop_front();
                break;
            case RejectPolicy::Throw:
                throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
            }
        }

        taskQueue_.emplace_back(std::move(task));
        submittedCount_.fetch_add(1, std::memory_order_relaxed);

        // 记录最近一次提交时间（统一使用纳秒）
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        lastSubmitNs_.store(now_ns, std::memory_order_relaxed);

        // 峰值队列
        const size_t qsz = taskQueue_.size();
        Math::UpdateMax(peakQueueSize_, qsz);

        queueNotEmptyCv_.notify_one();

        // 动态扩容：队列长度 > 活跃线程数 且还没到 maxThreads
        if (opts_.allowDynamicResize && running_.load(std::memory_order_acquire)) {
            size_t alive = aliveThreads_.load(std::memory_order_acquire);
            if (alive < opts_.maxThreads && qsz > alive) {
                SpawnWorker();
            }
        }
        return true;
    }

    void ThreadPool::WorkerLoop() {
        // 线程命名（可选）
        if (!opts_.threadNamePrefix.Empty()) {
            std::wostringstream woss;
            // 获取当前线程的 ID 并直接转换为整数（通常是一个指针值）
            std::thread::id threadId = std::this_thread::get_id();

            // 将 std::thread::id 转换为整数值（指针值），直接作为唯一标识符
            std::uintptr_t idValue = reinterpret_cast<std::uintptr_t>(&threadId);

            // 保证生成一个 5 位数 ID（通过对 idValue 进行模运算）
            size_t threadIdNum = idValue % 100000;  // 保证为 5 位数

            opts_.threadNamePrefix = opts_.threadNamePrefix.SubString(0, 15 - 5);

            // 确保输出为 5 位数（不足前面补零）
            woss << std::setw(5) << std::setfill(L'0') << threadIdNum;
            String threadName;
            threadName.Append(opts_.threadNamePrefix).Append(String(woss.str()));
            CoreUtils::SetCurrentThreadName(threadName);
        }

        // 更新历史最大线程数（spawnWorker 已把 aliveThreads_++）
        {
            size_t cur = aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(largestPoolSize_, cur);
        }

        auto tryExit = [this]() -> bool {
            if (shutdownNowFlag_.load(std::memory_order_acquire)) return true;
            if (!running_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(queueMutex_);
                return taskQueue_.empty(); // drain 完队列即可退出
            }
            return false;
            };

        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(queueMutex_);

                if (!shutdownNowFlag_.load(std::memory_order_acquire)) {
                    // 空闲等待，带 keepAlive，用于缩容
                    queueNotEmptyCv_.wait_for(lock, opts_.keepAlive, [&] {
                        return !taskQueue_.empty() || shutdownNowFlag_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire);
                        });
                }

                // CancelNow 直接退
                if (shutdownNowFlag_.load(std::memory_order_acquire)) break;

                // Graceful/Drain：队列空且不再运行 -> 退出
                if (!running_.load(std::memory_order_acquire) && taskQueue_.empty()) break;

                // 动态缩容：超时空闲且线程数超过 core -> 退出
                if (taskQueue_.empty() && opts_.allowDynamicResize && aliveThreads_.load(std::memory_order_acquire) > opts_.coreThreads) {
                    break;
                }

                if (!taskQueue_.empty()) {
                    task = std::move(taskQueue_.front());
                    taskQueue_.pop_front();
                    queueNotFullCv_.notify_one();
                }
            }

            if (task) {
                activeCount_.fetch_add(1, std::memory_order_relaxed);
                Timer timer(true, &timer_);
                try {
                    task();
                }
                catch (...) {
                    if (opts_.exceptionHandler) {
                        try { opts_.exceptionHandler(std::current_exception()); }
                        catch (...) {} // 回调自己异常也吞掉，避免杀死worker
                    }
                }
                auto elapsed = timer.Stop(); // 自动更新全局 EMA 和最长耗时
                completedCount_.fetch_add(1, std::memory_order_relaxed);
                activeCount_.fetch_sub(1, std::memory_order_relaxed);
                lastFinishNs_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count(), std::memory_order_relaxed
                );
            }

            if (tryExit()) break;
        }

        // 线程退出
        {
            std::lock_guard<std::mutex> lk(workerExitMutex_);
            long long prev = aliveThreads_.load(std::memory_order_acquire);
            aliveThreads_.store((prev > 0 ? prev - 1 : 0), std::memory_order_release);
            if (aliveThreads_.load(std::memory_order_acquire) == 0) workerExitCv_.notify_all();
        }
    }

    void ThreadPool::SpawnWorker() {
        {
            std::lock_guard<std::mutex> lk(workerExitMutex_);
            aliveThreads_.fetch_add(1, std::memory_order_relaxed);
            size_t cur = aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(largestPoolSize_, cur);
        }

        std::thread t([this] { WorkerLoop(); });
        {
            std::lock_guard<std::mutex> lk(workersMutex_);
            workers_.emplace_back(std::move(t));
        }
    }

    void ThreadPool::NotifyAllWorkers() {
        queueNotEmptyCv_.notify_all();
        queueNotFullCv_.notify_all();
    }
}