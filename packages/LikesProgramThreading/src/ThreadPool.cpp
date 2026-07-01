#include <LikesProgram/Threading/ThreadPool.hpp>
#include "threading/ThreadName.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace LikesProgram {
    namespace Threading {
        namespace {
            void UpdateMax(std::atomic<size_t>& target, size_t value) {
                size_t current = target.load(std::memory_order_relaxed); // 当前最大值快照
                while (value > current &&
                    !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                    // current 会被 compare_exchange_weak 刷成最新值。
                }
            }
        }

        struct ThreadPool::ThreadPoolImpl {
            Options m_options;                                  // 线程池配置快照
            mutable std::mutex m_queueMutex;                    // 保护任务队列
            std::condition_variable m_queueNotEmptyCv;          // 通知 worker 有任务或关闭
            std::condition_variable m_queueNotFullCv;           // 通知提交线程队列有空位
            std::deque<std::function<void()>> m_taskQueue;      // 待执行任务队列
            size_t m_queueCapacity = 1;                         // 队列容量，下限为 1

            std::vector<std::thread> m_workers;                 // worker 线程集合，JoinAll 后清空
            mutable std::mutex m_workersMutex;                  // 保护 worker 集合

            std::atomic<bool> m_running{ false };               // worker 是否继续取任务
            std::atomic<bool> m_acceptTasks{ false };           // 是否接受新任务提交
            std::atomic<bool> m_shutdownNow{ false };           // CancelNow 快速退出标志

            std::atomic<size_t> m_submittedCount{ 0 };          // 成功提交任务数
            std::atomic<size_t> m_rejectedCount{ 0 };           // 拒绝或取消任务数
            std::atomic<size_t> m_completedCount{ 0 };          // 完成任务数
            std::atomic<size_t> m_peakQueueSize{ 0 };           // 历史最大队列长度
            std::atomic<size_t> m_activeTasks{ 0 };             // 正在执行任务数
            std::atomic<size_t> m_aliveThreads{ 0 };            // 存活 worker 数
            std::atomic<size_t> m_largestPoolSize{ 0 };         // 历史最大 worker 数
            std::atomic<int64_t> m_lastSubmitNs{ 0 };           // 最近提交时间，system_clock 纳秒
            std::atomic<int64_t> m_lastFinishNs{ 0 };           // 最近完成时间，system_clock 纳秒

            std::shared_ptr<IThreadPoolObserver> m_observer;    // 可选观察者，不依赖 Metrics
            mutable std::mutex m_workerExitMutex;               // 保护退出等待条件
            std::condition_variable m_workerExitCv;             // 通知 AwaitTermination
        };

        ThreadPool::ThreadPool(std::shared_ptr<IThreadPoolObserver> observer, Options options)
            : m_impl(new ThreadPoolImpl{}) {
            // 构造阶段只保存配置，不自动启动，保持旧版显式 Start 语义。
            m_impl->m_observer = std::move(observer);
            m_impl->m_options = std::move(options);
            m_impl->m_queueCapacity = std::max<size_t>(1, m_impl->m_options.queueCapacity);
        }

        ThreadPool::ThreadPool(Options options)
            : ThreadPool(nullptr, std::move(options)) {
        }

        ThreadPool::ThreadPool(std::shared_ptr<IThreadPoolObserver> observer)
            : ThreadPool(std::move(observer), Options()) {
        }

        ThreadPool::ThreadPool()
            : ThreadPool(nullptr, Options()) {
        }

        ThreadPool::~ThreadPool() {
            try {
                // 析构走 CancelNow，避免对象销毁时继续持有用户任务。
                Shutdown(ShutdownPolicy::CancelNow);
                NotifyAllWorkers();
                JoinAll();
            }
            catch (...) {
                // 析构不能抛出异常。
            }

            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

        void ThreadPool::Start() {
            bool expected = false; // compare_exchange 的未启动期望值
            if (!m_impl->m_running.compare_exchange_strong(expected, true)) return;

            // 启动新周期时允许提交任务，并清掉上一次 CancelNow 标记。
            m_impl->m_shutdownNow.store(false, std::memory_order_release);
            m_impl->m_acceptTasks.store(true, std::memory_order_release);
            for (size_t i = 0; i < m_impl->m_options.coreThreads; ++i) {
                if (!SpawnWorker()) {
                    // 核心 worker 无法创建时回滚运行态，避免留下可提交但无 worker 的线程池。
                    m_impl->m_acceptTasks.store(false, std::memory_order_release);
                    m_impl->m_running.store(false, std::memory_order_release);
                    NotifyAllWorkers();
                    JoinAll();
                    throw std::runtime_error("ThreadPool: failed to start worker thread");
                }
            }
        }

        void ThreadPool::Shutdown(ShutdownPolicy mode) {
            if (!m_impl) return;
            if (!m_impl->m_running.load(std::memory_order_acquire) &&
                !m_impl->m_acceptTasks.load(std::memory_order_acquire)) {
                return;
            }

            // 所有关闭策略都停止接收新任务；CancelNow 额外清队列。
            m_impl->m_acceptTasks.store(false, std::memory_order_release);
            m_impl->m_running.store(false, std::memory_order_release);

            if (mode == ShutdownPolicy::CancelNow) {
                m_impl->m_shutdownNow.store(true, std::memory_order_release);
                size_t canceled = 0; // 被清掉的等待任务数
                {
                    std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
                    canceled = m_impl->m_taskQueue.size();
                    m_impl->m_taskQueue.clear();
                }

                if (canceled > 0) {
                    m_impl->m_rejectedCount.fetch_add(canceled, std::memory_order_relaxed);
                    ThreadPoolEvent event = MakeEvent();
                    for (size_t i = 0; i < canceled; ++i) NotifyTaskRejected(event);
                }
            }

            NotifyAllWorkers();
        }

        bool ThreadPool::AwaitTermination(std::chrono::milliseconds timeout) {
            if (!m_impl) return true;
            if (timeout.count() == 0) {
                return m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0;
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout; // 等待截止时间
            std::unique_lock<std::mutex> lock(m_impl->m_workerExitMutex);
            return m_impl->m_workerExitCv.wait_until(lock, deadline, [this] {
                return m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0;
            });
        }

        bool ThreadPool::PostNoArg(std::function<void()> function) {
            bool success = EnqueueTask(std::move(function)); // 入队结果
            if (!success) ReportException(std::make_exception_ptr(std::runtime_error("Task rejected")));
            return success;
        }

        size_t ThreadPool::GetQueueSize() const {
            std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
            return m_impl->m_taskQueue.size();
        }

        size_t ThreadPool::GetActiveCount() const {
            return m_impl->m_activeTasks.load(std::memory_order_acquire);
        }

        size_t ThreadPool::GetThreadCount() const {
            return m_impl->m_aliveThreads.load(std::memory_order_acquire);
        }

        bool ThreadPool::IsRunning() const {
            return m_impl->m_acceptTasks.load(std::memory_order_acquire);
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
            Statistics stats;
            // 统计快照只读取原子值，不阻塞提交路径。
            stats.submitted = m_impl->m_submittedCount.load(std::memory_order_acquire);
            stats.rejected = m_impl->m_rejectedCount.load(std::memory_order_acquire);
            stats.completed = m_impl->m_completedCount.load(std::memory_order_acquire);
            stats.active = m_impl->m_activeTasks.load(std::memory_order_acquire);
            stats.aliveThreads = m_impl->m_aliveThreads.load(std::memory_order_acquire);
            stats.largestPoolSize = m_impl->m_largestPoolSize.load(std::memory_order_acquire);
            stats.peakQueueSize = m_impl->m_peakQueueSize.load(std::memory_order_acquire);

            const int64_t lastSubmitNs = m_impl->m_lastSubmitNs.load(std::memory_order_acquire);
            const int64_t lastFinishNs = m_impl->m_lastFinishNs.load(std::memory_order_acquire);
            if (lastSubmitNs > 0) stats.lastSubmitTime = Time::NsToSystemClock(lastSubmitNs);
            if (lastFinishNs > 0) stats.lastFinishTime = Time::NsToSystemClock(lastFinishNs);
            return stats;
        }

        void ThreadPool::JoinAll() {
            std::lock_guard<std::mutex> lock(m_impl->m_workersMutex);
            for (auto& worker : m_impl->m_workers) {
                if (worker.joinable()) worker.join();
            }
            m_impl->m_workers.clear();
        }

        bool ThreadPool::EnqueueTask(std::function<void()>&& task) {
            if (!m_impl->m_running.load(std::memory_order_acquire) ||
                !m_impl->m_acceptTasks.load(std::memory_order_acquire)) {
                m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                NotifyTaskRejected(MakeEvent());
                return false;
            }

            std::unique_lock<std::mutex> lock(m_impl->m_queueMutex);
            if (!m_impl->m_running.load(std::memory_order_acquire) ||
                !m_impl->m_acceptTasks.load(std::memory_order_acquire)) {
                // 关闭可能发生在首次快速检查和真正持锁入队之间，需要二次确认。
                m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                const size_t queueSize = m_impl->m_taskQueue.size();
                lock.unlock();
                NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                return false;
            }

            bool droppedOldTask = false; // DiscardOld 策略是否丢弃了队头任务
            size_t droppedOldQueueSize = 0; // 丢弃后队列长度，用于释放锁后发事件
            if (m_impl->m_taskQueue.size() >= m_impl->m_queueCapacity) {
                switch (m_impl->m_options.rejectPolicy) {
                case RejectPolicy::Block:
                    m_impl->m_queueNotFullCv.wait(lock, [this] {
                        return m_impl->m_taskQueue.size() < m_impl->m_queueCapacity ||
                            !m_impl->m_acceptTasks.load(std::memory_order_acquire) ||
                            m_impl->m_shutdownNow.load(std::memory_order_acquire);
                    });
                    if (!m_impl->m_acceptTasks.load(std::memory_order_acquire) ||
                        m_impl->m_shutdownNow.load(std::memory_order_acquire)) {
                        m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 持锁读取，避免 MakeEvent 重入队列锁
                        lock.unlock();
                        NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                        return false;
                    }
                    break;
                case RejectPolicy::Discard:
                    m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                    {
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 拒绝时队列长度不变
                        lock.unlock();
                        NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                    }
                    return false;
                case RejectPolicy::DiscardOld:
                    if (!m_impl->m_taskQueue.empty()) {
                        m_impl->m_taskQueue.pop_front();
                        m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                        droppedOldTask = true;
                        droppedOldQueueSize = m_impl->m_taskQueue.size();
                    }
                    break;
                case RejectPolicy::Throw:
                    m_impl->m_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                    {
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 抛出前保留事件快照
                        lock.unlock();
                        NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                    }
                    throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
                }
            }

            m_impl->m_taskQueue.emplace_back(std::move(task));
            const size_t queueSize = m_impl->m_taskQueue.size(); // 入队后的队列长度
            m_impl->m_submittedCount.fetch_add(1, std::memory_order_relaxed);
            UpdateMax(m_impl->m_peakQueueSize, queueSize);
            m_impl->m_lastSubmitNs.store(Time::SystemClockToDuration(std::chrono::system_clock::now()).count(),
                std::memory_order_relaxed);

            lock.unlock();
            m_impl->m_queueNotEmptyCv.notify_one();
            if (droppedOldTask) NotifyTaskRejected(MakeEventWithQueueSize(droppedOldQueueSize));
            NotifyTaskSubmitted(MakeEvent());

            // 动态扩容在释放队列锁后执行，避免创建线程时阻塞提交路径。
            if (m_impl->m_options.allowDynamicResize && m_impl->m_running.load(std::memory_order_acquire)) {
                const size_t alive = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                if (alive < m_impl->m_options.maxThreads && queueSize > alive) {
                    (void)SpawnWorker();
                }
            }
            return true;
        }

        void ThreadPool::WorkerLoop() {
            if (!m_impl->m_options.threadNamePrefix.Empty()) {
                std::ostringstream suffix; // 线程名后缀，便于诊断区分 worker
                suffix << std::setw(5) << std::setfill('0')
                    << (std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000);
                Detail::SetCurrentThreadName(m_impl->m_options.threadNamePrefix + String(suffix.str()));
            }

            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_impl->m_queueMutex);
                    if (!m_impl->m_shutdownNow.load(std::memory_order_acquire)) {
                        m_impl->m_queueNotEmptyCv.wait_for(lock, m_impl->m_options.keepAlive, [this] {
                            return !m_impl->m_taskQueue.empty() ||
                                m_impl->m_shutdownNow.load(std::memory_order_acquire) ||
                                !m_impl->m_running.load(std::memory_order_acquire);
                        });
                    }

                    if (m_impl->m_shutdownNow.load(std::memory_order_acquire)) break;
                    if (!m_impl->m_running.load(std::memory_order_acquire) && m_impl->m_taskQueue.empty()) break;
                    if (m_impl->m_taskQueue.empty() &&
                        m_impl->m_options.allowDynamicResize &&
                        m_impl->m_aliveThreads.load(std::memory_order_acquire) > m_impl->m_options.coreThreads) {
                        break;
                    }

                    if (!m_impl->m_taskQueue.empty()) {
                        task = std::move(m_impl->m_taskQueue.front());
                        m_impl->m_taskQueue.pop_front();
                        m_impl->m_queueNotFullCv.notify_one();
                    }
                }

                if (!task) continue;

                m_impl->m_activeTasks.fetch_add(1, std::memory_order_relaxed);
                NotifyTaskStarted(MakeEvent());
                const auto start = std::chrono::steady_clock::now(); // 任务耗时起点

                try {
                    task();
                }
                catch (...) {
                    ReportException(std::current_exception());
                }

                const auto elapsed = std::chrono::duration_cast<Time::Nanoseconds>(
                    std::chrono::steady_clock::now() - start);
                m_impl->m_completedCount.fetch_add(1, std::memory_order_relaxed);
                m_impl->m_activeTasks.fetch_sub(1, std::memory_order_relaxed);
                m_impl->m_lastFinishNs.store(Time::SystemClockToDuration(std::chrono::system_clock::now()).count(),
                    std::memory_order_relaxed);
                NotifyTaskCompleted(MakeEvent(elapsed));
            }

            bool notifyExit = false; // 退出事件在释放锁后通知观察者
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                const size_t previous = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                m_impl->m_aliveThreads.store(previous > 0 ? previous - 1 : 0, std::memory_order_release);
                notifyExit = true;
                if (m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0) {
                    m_impl->m_workerExitCv.notify_all();
                }
            }
            if (notifyExit) NotifyThreadCountRemoved(MakeEvent());
        }

        bool ThreadPool::SpawnWorker() {
            size_t current = 0; // 新 worker 预留后的存活线程数
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                if (!m_impl->m_running.load(std::memory_order_acquire)) {
                    return false;
                }
                if (m_impl->m_aliveThreads.load(std::memory_order_acquire) >= m_impl->m_options.maxThreads) {
                    return false;
                }
                current = m_impl->m_aliveThreads.fetch_add(1, std::memory_order_relaxed) + 1;
                UpdateMax(m_impl->m_largestPoolSize, current);
            }

            {
                std::lock_guard<std::mutex> workersLock(m_impl->m_workersMutex);
                try {
                    m_impl->m_workers.emplace_back([this] {
                        WorkerLoop();
                    });
                }
                catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                        const size_t previous = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                        m_impl->m_aliveThreads.store(previous > 0 ? previous - 1 : 0, std::memory_order_release);
                        if (m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0) {
                            m_impl->m_workerExitCv.notify_all();
                        }
                    }
                    ReportException(std::current_exception());
                    return false;
                }
            }
            NotifyThreadCountAdded(MakeEvent());
            return true;
        }

        void ThreadPool::NotifyAllWorkers() {
            m_impl->m_queueNotEmptyCv.notify_all();
            m_impl->m_queueNotFullCv.notify_all();
        }

        std::function<void(std::exception_ptr)> ThreadPool::GetExceptionHandler() const {
            return m_impl->m_options.exceptionHandler;
        }

        void ThreadPool::ReportException(std::exception_ptr error) const {
            auto handler = GetExceptionHandler(); // 处理器快照，避免回调期间配置变化
            if (!handler) return;
            try {
                handler(error);
            }
            catch (...) {
                // 用户异常处理器自身异常被隔离，避免杀死 worker。
            }
        }

        ThreadPoolEvent ThreadPool::MakeEvent(Time::Nanoseconds duration) const {
            return MakeEventWithQueueSize(GetQueueSize(), duration);
        }

        ThreadPoolEvent ThreadPool::MakeEventWithQueueSize(size_t queueSize, Time::Nanoseconds duration) const {
            ThreadPoolEvent event;
            event.queueSize = queueSize;
            event.activeTasks = m_impl->m_activeTasks.load(std::memory_order_acquire);
            event.aliveThreads = m_impl->m_aliveThreads.load(std::memory_order_acquire);
            event.duration = duration;
            event.timestamp = std::chrono::system_clock::now();
            return event;
        }

        void ThreadPool::NotifyTaskSubmitted(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnTaskSubmitted(event); }
            catch (...) { ReportException(std::current_exception()); }
        }

        void ThreadPool::NotifyTaskRejected(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnTaskRejected(event); }
            catch (...) { ReportException(std::current_exception()); }
        }

        void ThreadPool::NotifyTaskStarted(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnTaskStarted(event); }
            catch (...) { ReportException(std::current_exception()); }
        }

        void ThreadPool::NotifyTaskCompleted(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnTaskCompleted(event); }
            catch (...) { ReportException(std::current_exception()); }
        }

        void ThreadPool::NotifyThreadCountAdded(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnThreadCountAdded(event); }
            catch (...) { ReportException(std::current_exception()); }
        }

        void ThreadPool::NotifyThreadCountRemoved(const ThreadPoolEvent& event) {
            auto observer = m_impl->m_observer; // shared_ptr 快照保护回调生命周期
            if (!observer) return;
            try { observer->OnThreadCountRemoved(event); }
            catch (...) { ReportException(std::current_exception()); }
        }
    }
}
