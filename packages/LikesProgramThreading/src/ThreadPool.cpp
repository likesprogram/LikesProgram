#include <LikesProgram/Threading/ThreadPool.hpp>
#include "threading/ThreadName.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace LikesProgram {
    namespace Threading {
        namespace {
            constexpr size_t kTimestampSampleInterval = 64; // 诊断墙钟的事件采样间隔
            constexpr size_t kWorkerActiveBit = 1;          // worker 统计状态最低位表示正在执行
            constexpr size_t kWorkerIdleSpinCount = 64;     // 短任务空队列交接前的有界让出次数

            bool ShouldSampleTimestamp(size_t eventCount) {
                // 第一项保证短生命周期有值，后续按固定幂次间隔降低热路径成本。
                return eventCount == 1 || eventCount % kTimestampSampleInterval == 0;
            }

            int64_t SystemNowNs() {
#ifdef _WIN32
                FILETIME fileTime{}; // Windows 1601 epoch 的 100 ns 系统时间
                GetSystemTimeAsFileTime(&fileTime);
                ULARGE_INTEGER ticks{}; // 合并高低 32 位 FILETIME
                ticks.LowPart = fileTime.dwLowDateTime;
                ticks.HighPart = fileTime.dwHighDateTime;
                constexpr uint64_t windowsToUnixEpoch = 116444736000000000ULL; // 两个 epoch 的 100 ns 差值
                if (ticks.QuadPart <= windowsToUnixEpoch) return 0;
                const uint64_t unixTicks = ticks.QuadPart - windowsToUnixEpoch; // Unix epoch 后的 100 ns 数
                constexpr uint64_t maxTicks = static_cast<uint64_t>(
                    std::numeric_limits<int64_t>::max()) / 100ULL; // 纳秒结果的安全上限
                if (unixTicks > maxTicks) return std::numeric_limits<int64_t>::max();
                return static_cast<int64_t>(unixTicks * 100ULL);
#else
                // 非 Windows 平台保留标准 system_clock 行为。
                return Time::SystemClockToDuration(std::chrono::system_clock::now()).count();
#endif
            }

            void UpdateMax(std::atomic<size_t>& target, size_t value) {
                size_t current = target.load(std::memory_order_relaxed); // 当前最大值快照
                while (value > current &&
                    !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                    // current 会被 compare_exchange_weak 刷成最新值。
                }
            }

        }

        struct ThreadPool::ThreadPoolImpl {
            struct alignas(64) WorkerStatistics {
                std::atomic<size_t> activityAndCompleted{ 0 };  // 高位完成数、最低位 active
                std::atomic<int64_t> lastFinishNs{ 0 };         // 最近采样完成时间，system_clock 纳秒
                bool inUse = false;                             // 槽位是否绑定存活 worker，由退出锁保护
                uint8_t padding[64 - sizeof(std::atomic<size_t>) - sizeof(std::atomic<int64_t>) -
                    sizeof(bool)]{};                            // 显式补齐单个缓存行，避免隐式尾填充
            };

            Options m_options;                                  // 线程池配置快照
            mutable std::mutex m_queueMutex;                    // 保护任务队列
            std::condition_variable m_queueNotEmptyCv;          // 通知 worker 有任务或关闭
            std::condition_variable m_queueNotFullCv;           // 通知提交线程队列有空位
            std::deque<Detail::ThreadPoolTask> m_taskQueue;     // 待执行的 move-only 任务队列
            size_t m_queueCapacity = 1;                         // 队列容量，下限为 1
            size_t m_waitingWorkers = 0;                        // 队列条件变量上的 worker 数，由队列锁保护

            std::vector<std::thread> m_workers;                 // worker 线程集合，JoinAll 后清空
            mutable std::mutex m_workersMutex;                  // 保护 worker 集合
            std::vector<std::unique_ptr<WorkerStatistics>> m_workerStatistics; // 可复用的 worker 统计槽位

            std::atomic<bool> m_running{ false };               // worker 是否继续取任务
            std::atomic<bool> m_acceptTasks{ false };           // 是否接受新任务提交
            std::atomic<bool> m_shutdownNow{ false };           // CancelNow 快速退出标志

            // 提交侧统计与任务队列共用锁，避免持锁热路径内重复原子同步。
            size_t m_submittedCount = 0;                        // 成功提交任务数
            size_t m_rejectedCount = 0;                         // 拒绝或取消任务数
            size_t m_peakQueueSize = 0;                         // 历史最大队列长度
            int64_t m_lastSubmitNs = 0;                         // 最近提交时间，system_clock 纳秒

            std::atomic<size_t> m_aliveThreads{ 0 };            // 存活 worker 数
            std::atomic<size_t> m_largestPoolSize{ 0 };         // 历史最大 worker 数

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

            // 状态变更与队列锁保持同一同步域，避免 worker 在 predicate 检查后错过关闭唤醒。
            size_t canceled = 0; // 被清掉的等待任务数
            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
                m_impl->m_acceptTasks.store(false, std::memory_order_release);
                m_impl->m_running.store(false, std::memory_order_release);

                if (mode == ShutdownPolicy::CancelNow) {
                    m_impl->m_shutdownNow.store(true, std::memory_order_release);
                    canceled = m_impl->m_taskQueue.size();
                    m_impl->m_taskQueue.clear();
                    m_impl->m_rejectedCount += canceled;
                }
            }

            if (canceled > 0) {
                // 未配置观察者时不构造事件快照，关闭统计仍保留。
                if (m_impl->m_observer) {
                    ThreadPoolEvent event = MakeEvent(); // 本轮取消任务共享的状态快照
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
            bool success = EnqueueTask(Detail::ThreadPoolTask(std::move(function))); // 入队结果
            if (!success) ReportException(std::make_exception_ptr(std::runtime_error("Task rejected")));
            return success;
        }

        size_t ThreadPool::GetQueueSize() const {
            std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
            return m_impl->m_taskQueue.size();
        }

        size_t ThreadPool::GetActiveCount() const {
            size_t active = 0; // 所有 worker 槽位的活动任务总数
            std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
            for (const auto& statistics : m_impl->m_workerStatistics) {
                if ((statistics->activityAndCompleted.load(std::memory_order_acquire) &
                    kWorkerActiveBit) != 0) ++active;
            }
            return active;
        }

        size_t ThreadPool::GetThreadCount() const {
            return m_impl->m_aliveThreads.load(std::memory_order_acquire);
        }

        bool ThreadPool::IsRunning() const {
            return m_impl->m_acceptTasks.load(std::memory_order_acquire);
        }

        size_t ThreadPool::IetRejectedCount() const {
            std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
            return m_impl->m_rejectedCount;
        }

        size_t ThreadPool::IetTotalTasksSubmitted() const {
            std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
            return m_impl->m_submittedCount;
        }

        size_t ThreadPool::IetCompletedCount() const {
            size_t completed = 0; // 所有 worker 槽位的累计完成数
            std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
            for (const auto& statistics : m_impl->m_workerStatistics) {
                completed += statistics->activityAndCompleted.load(std::memory_order_acquire) >> 1;
            }
            return completed;
        }

        size_t ThreadPool::IetLargestPoolSize() const {
            return m_impl->m_largestPoolSize.load(std::memory_order_acquire);
        }

        size_t ThreadPool::IetPeakQueueSize() const {
            std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
            return m_impl->m_peakQueueSize;
        }

        ThreadPool::Statistics ThreadPool::Snapshot() const {
            Statistics stats;
            int64_t lastSubmitNs = 0; // 队列锁域内读取的最近提交时间
            {
                std::lock_guard<std::mutex> lock(m_impl->m_queueMutex);
                stats.submitted = m_impl->m_submittedCount;
                stats.rejected = m_impl->m_rejectedCount;
                stats.peakQueueSize = m_impl->m_peakQueueSize;
                lastSubmitNs = m_impl->m_lastSubmitNs;
            }
            int64_t lastFinishNs = 0; // 所有 worker 槽位中的最新采样墙钟
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                for (const auto& statistics : m_impl->m_workerStatistics) {
                    const size_t state = statistics->activityAndCompleted.load(
                        std::memory_order_acquire); // 同一时刻的完成数与 active 快照
                    stats.completed += state >> 1;
                    if ((state & kWorkerActiveBit) != 0) ++stats.active;
                    lastFinishNs = std::max(lastFinishNs,
                        statistics->lastFinishNs.load(std::memory_order_acquire));
                }
                stats.aliveThreads = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                stats.largestPoolSize = m_impl->m_largestPoolSize.load(std::memory_order_acquire);
            }

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
            // 旧 consumer 仍可调用原导出符号，进入相同 move-only 队列实现。
            return EnqueueTask(Detail::ThreadPoolTask(std::move(task)));
        }

        bool ThreadPool::EnqueueTask(Detail::ThreadPoolTask&& task) {
            std::unique_lock<std::mutex> lock(m_impl->m_queueMutex);
            if (!m_impl->m_running.load(std::memory_order_acquire) ||
                !m_impl->m_acceptTasks.load(std::memory_order_acquire)) {
                ++m_impl->m_rejectedCount;
                const size_t queueSize = m_impl->m_taskQueue.size();
                lock.unlock();
                if (m_impl->m_observer) NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
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
                        ++m_impl->m_rejectedCount;
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 持锁读取，避免 MakeEvent 重入队列锁
                        lock.unlock();
                        if (m_impl->m_observer) NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                        return false;
                    }
                    break;
                case RejectPolicy::Discard:
                    ++m_impl->m_rejectedCount;
                    {
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 拒绝时队列长度不变
                        lock.unlock();
                        if (m_impl->m_observer) NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                    }
                    return false;
                case RejectPolicy::DiscardOld:
                    if (!m_impl->m_taskQueue.empty()) {
                        m_impl->m_taskQueue.pop_front();
                        ++m_impl->m_rejectedCount;
                        droppedOldTask = true;
                        droppedOldQueueSize = m_impl->m_taskQueue.size();
                    }
                    break;
                case RejectPolicy::Throw:
                    ++m_impl->m_rejectedCount;
                    {
                        const size_t queueSize = m_impl->m_taskQueue.size(); // 抛出前保留事件快照
                        lock.unlock();
                        if (m_impl->m_observer) NotifyTaskRejected(MakeEventWithQueueSize(queueSize));
                    }
                    throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
                }
            }

            m_impl->m_taskQueue.emplace_back(std::move(task));
            const size_t queueSize = m_impl->m_taskQueue.size(); // 入队后的队列长度
            const bool notifyWaitingWorker = m_impl->m_waitingWorkers > 0; // 是否存在真实条件等待者
            ++m_impl->m_submittedCount;
            if (queueSize > m_impl->m_peakQueueSize) m_impl->m_peakQueueSize = queueSize;
            if (ShouldSampleTimestamp(m_impl->m_submittedCount)) {
                m_impl->m_lastSubmitNs = SystemNowNs();
            }

            lock.unlock();
            if (notifyWaitingWorker) m_impl->m_queueNotEmptyCv.notify_one();
            // observer 是可选能力，默认热路径不承担事件快照和队列重入锁成本。
            if (m_impl->m_observer) {
                if (droppedOldTask) NotifyTaskRejected(MakeEventWithQueueSize(droppedOldQueueSize));
                NotifyTaskSubmitted(MakeEvent());
            }

            // 动态扩容在释放队列锁后执行，避免创建线程时阻塞提交路径。
            if (m_impl->m_options.allowDynamicResize && m_impl->m_running.load(std::memory_order_acquire)) {
                const size_t alive = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                if (alive < m_impl->m_options.maxThreads && queueSize > alive) {
                    (void)SpawnWorker();
                }
            }
            return true;
        }

        void ThreadPool::WorkerLoop(size_t statisticsSlot) {
            ThreadPoolImpl::WorkerStatistics* workerStatistics = nullptr; // 当前 worker 独占的统计槽位
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                workerStatistics = m_impl->m_workerStatistics.at(statisticsSlot).get();
            }
            size_t workerState = workerStatistics->activityAndCompleted.load(
                std::memory_order_relaxed) & ~kWorkerActiveBit; // 当前 worker 独占更新的累计状态

            if (!m_impl->m_options.threadNamePrefix.Empty()) {
                std::ostringstream suffix; // 线程名后缀，便于诊断区分 worker
                suffix << std::setw(5) << std::setfill('0')
                    << (std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000);
                Detail::SetCurrentThreadName(m_impl->m_options.threadNamePrefix + String(suffix.str()));
            }

            while (true) {
                Detail::ThreadPoolTask task; // 本轮从队列接管的 move-only 任务
                bool notifyQueueSpace = false; // 本轮是否从满队列释放了一个可提交槽位
                {
                    std::unique_lock<std::mutex> lock(m_impl->m_queueMutex);
                    // 非空队列直接取任务，避免热路径重复建立 keep-alive deadline。
                    if (!m_impl->m_shutdownNow.load(std::memory_order_acquire) &&
                        m_impl->m_taskQueue.empty()) {
                        for (size_t spin = 0; spin < kWorkerIdleSpinCount; ++spin) {
                            lock.unlock();
                            std::this_thread::yield();
                            lock.lock();
                            if (!m_impl->m_taskQueue.empty() ||
                                m_impl->m_shutdownNow.load(std::memory_order_acquire) ||
                                !m_impl->m_running.load(std::memory_order_acquire)) {
                                break;
                            }
                        }
                    }
                    if (!m_impl->m_shutdownNow.load(std::memory_order_acquire) &&
                        m_impl->m_taskQueue.empty() &&
                        m_impl->m_running.load(std::memory_order_acquire)) {
                        ++m_impl->m_waitingWorkers;
                        m_impl->m_queueNotEmptyCv.wait_for(lock, m_impl->m_options.keepAlive, [this] {
                            return !m_impl->m_taskQueue.empty() ||
                                m_impl->m_shutdownNow.load(std::memory_order_acquire) ||
                                !m_impl->m_running.load(std::memory_order_acquire);
                        });
                        --m_impl->m_waitingWorkers;
                    }

                    if (m_impl->m_shutdownNow.load(std::memory_order_acquire)) break;
                    if (!m_impl->m_running.load(std::memory_order_acquire) && m_impl->m_taskQueue.empty()) break;
                    if (m_impl->m_taskQueue.empty() &&
                        m_impl->m_options.allowDynamicResize &&
                        m_impl->m_aliveThreads.load(std::memory_order_acquire) > m_impl->m_options.coreThreads) {
                        break;
                    }

                    if (!m_impl->m_taskQueue.empty()) {
                        // 只有满队列才可能存在 Block 提交者，避免普通热路径触发无效唤醒。
                        notifyQueueSpace = m_impl->m_taskQueue.size() >= m_impl->m_queueCapacity;
                        task = std::move(m_impl->m_taskQueue.front());
                        m_impl->m_taskQueue.pop_front();
                    }
                }
                if (notifyQueueSpace) m_impl->m_queueNotFullCv.notify_one();

                if (!task) continue;

                workerStatistics->activityAndCompleted.store(
                    workerState | kWorkerActiveBit, std::memory_order_release);
                const bool observeTask = static_cast<bool>(m_impl->m_observer); // 本任务是否需要事件和耗时
                if (observeTask) NotifyTaskStarted(MakeEvent());
                const auto start = observeTask ? std::chrono::steady_clock::now() :
                    std::chrono::steady_clock::time_point{}; // 仅观察路径记录任务耗时

                try {
                    task();
                }
                catch (...) {
                    ReportException(std::current_exception());
                }

                workerState += 2;
                workerStatistics->activityAndCompleted.store(workerState, std::memory_order_release);
                const size_t completed = workerState >> 1; // 当前槽位累计完成任务数
                if (ShouldSampleTimestamp(completed)) {
                    workerStatistics->lastFinishNs.store(SystemNowNs(), std::memory_order_relaxed);
                }
                // 未配置观察者时不读取 steady_clock，也不构造完成事件。
                if (observeTask) {
                    const auto elapsed = std::chrono::duration_cast<Time::Nanoseconds>(
                        std::chrono::steady_clock::now() - start); // 观察者需要的任务耗时
                    NotifyTaskCompleted(MakeEvent(elapsed));
                }
            }

            bool notifyExit = false; // 退出事件在释放锁后通知观察者
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                workerStatistics->activityAndCompleted.store(workerState, std::memory_order_release);
                workerStatistics->inUse = false;
                const size_t previous = m_impl->m_aliveThreads.load(std::memory_order_acquire);
                m_impl->m_aliveThreads.store(previous > 0 ? previous - 1 : 0, std::memory_order_release);
                notifyExit = true;
                if (m_impl->m_aliveThreads.load(std::memory_order_acquire) == 0) {
                    m_impl->m_workerExitCv.notify_all();
                }
            }
            if (notifyExit && m_impl->m_observer) NotifyThreadCountRemoved(MakeEvent());
        }

        bool ThreadPool::SpawnWorker() {
            size_t current = 0; // 新 worker 预留后的存活线程数
            size_t statisticsSlot = 0; // 本轮 worker 独占统计槽位索引
            {
                std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                if (!m_impl->m_running.load(std::memory_order_acquire)) {
                    return false;
                }
                if (m_impl->m_aliveThreads.load(std::memory_order_acquire) >= m_impl->m_options.maxThreads) {
                    return false;
                }
                auto reusable = std::find_if(m_impl->m_workerStatistics.begin(),
                    m_impl->m_workerStatistics.end(), [](const auto& statistics) {
                        return !statistics->inUse;
                    });
                if (reusable == m_impl->m_workerStatistics.end()) {
                    m_impl->m_workerStatistics.push_back(
                        std::make_unique<ThreadPoolImpl::WorkerStatistics>());
                    statisticsSlot = m_impl->m_workerStatistics.size() - 1;
                }
                else {
                    statisticsSlot = static_cast<size_t>(
                        reusable - m_impl->m_workerStatistics.begin());
                }
                m_impl->m_workerStatistics[statisticsSlot]->inUse = true;
                const size_t reusableState = m_impl->m_workerStatistics[statisticsSlot]->
                    activityAndCompleted.load(std::memory_order_relaxed) & ~kWorkerActiveBit; // 清除旧 active 位
                m_impl->m_workerStatistics[statisticsSlot]->activityAndCompleted.store(
                    reusableState, std::memory_order_relaxed);
                current = m_impl->m_aliveThreads.fetch_add(1, std::memory_order_relaxed) + 1;
                UpdateMax(m_impl->m_largestPoolSize, current);
            }

            {
                std::lock_guard<std::mutex> workersLock(m_impl->m_workersMutex);
                try {
                    m_impl->m_workers.emplace_back([this, statisticsSlot] {
                        WorkerLoop(statisticsSlot);
                    });
                }
                catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(m_impl->m_workerExitMutex);
                        m_impl->m_workerStatistics[statisticsSlot]->inUse = false;
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
            if (m_impl->m_observer) NotifyThreadCountAdded(MakeEvent());
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
            event.activeTasks = GetActiveCount();
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
