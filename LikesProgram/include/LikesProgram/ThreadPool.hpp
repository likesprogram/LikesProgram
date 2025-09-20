#pragma once
#include "LikesProgramLibExport.hpp"
#include "String.hpp"
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
#include "Timer.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <stringapiset.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <pthread.h>
#endif

namespace LikesProgram {
    class LIKESPROGRAM_API ThreadPool { 
    public:
        // 任务拒绝策略
        enum class RejectPolicy {
            Block,       // 阻塞等待队列有空位
            Discard,     // 丢弃新任务
            DiscardOld,  // 丢弃最老任务（队头），然后入队新任务
            Throw        // 抛出异常
        };
        // 关闭策略
        enum class ShutdownPolicy {
            Graceful,   // 不接收新任务；执行完队列和在跑的任务再退出
            Drain,      // 立刻拒绝新任务；执行队列里已有任务；尽快退出（不清队列）
            CancelNow   // 立刻拒绝新任务；丢弃队列；尽快退出
        };
        // 配置选项
        struct Options {
            size_t coreThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1; // 最少线程
            size_t maxThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1;  // 最大线程
            size_t queueCapacity = 1024; // 队列容量
            RejectPolicy rejectPolicy = RejectPolicy::Block; // 拒绝策略
            std::chrono::milliseconds keepAlive = std::chrono::milliseconds(30000); // 空闲线程回收时间
            bool allowDynamicResize = true; // 是否启用动态扩容/回收
            String threadNamePrefix = u"tp-worker-"; // 线程名前缀
            std::function<void(std::exception_ptr)> exceptionHandler = [](std::exception_ptr) {}; // 异常回调
            // 构造函数
            Options(size_t coreThreads = 0, size_t maxThreads = 0, size_t queueCapacity = 1024,
                RejectPolicy rejectPolicy = RejectPolicy::Block,
                std::chrono::milliseconds keepAlive = std::chrono::milliseconds(30000),
                bool allowDynamicResize = true, String threadNamePrefix = u"tp-worker-",
                std::function<void(std::exception_ptr)> exceptionHandler = [](std::exception_ptr) {})
                : coreThreads(coreThreads ? coreThreads : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)),
                maxThreads(maxThreads ? maxThreads : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)),
                queueCapacity(queueCapacity), rejectPolicy(rejectPolicy), keepAlive(keepAlive), allowDynamicResize(allowDynamicResize),
                threadNamePrefix(std::move(threadNamePrefix)), exceptionHandler(std::move(exceptionHandler)) {
            }
        };

        // 运行时统计信息
        struct Statistics {
            size_t submitted = 0;       // 成功入队的任务数
            size_t rejected = 0;        // 被拒绝的任务数
            size_t completed = 0;       // 执行完成的任务数
            size_t active = 0;          // 正在执行的任务数
            size_t aliveThreads = 0;    // 存活工作线程数
            size_t largestPoolSize = 0; // 历史最大线程数
            size_t peakQueueSize = 0;   // 队列峰值
            std::chrono::steady_clock::time_point lastSubmitTime{};
            std::chrono::steady_clock::time_point lastFinishTime{};
            std::chrono::nanoseconds longestTaskTime{ 0 };
            std::chrono::nanoseconds arithmeticAverageTaskTime{ 0 };
            std::chrono::nanoseconds averageTaskTime{ 0 }; // EMA
        };

        explicit ThreadPool(Options opts = Options())
            : opts_(std::move(opts)),
            queueCapacity_(opts_.queueCapacity), timer_(Timer()) {
        }

        ~ThreadPool();

        // ---- 生命周期控制 ----
        // 启动线程池（可多次调用但仅第一次生效）
        void Start();

        // 统一的停止入口；兼容原 stop()/shutdownNow()
        void Shutdown(ShutdownPolicy mode = ShutdownPolicy::Graceful);

        // 兼容老接口
        void Stop() { Shutdown(ShutdownPolicy::Graceful); }
        void ShutdownNow() { Shutdown(ShutdownPolicy::CancelNow); }

        // 等待所有worker退出；timeout=0 表示非阻塞检查
        bool AwaitTermination(std::chrono::milliseconds timeout);

        // ---- 提交任务 ----
        // 提交一个任务,有参数和返回值
        template<typename F, typename... Args>
        auto Submit(F&& f, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>> {
            using Ret = std::invoke_result_t<F, Args...>;

            // packaged_task that invokes fn with moved args (supports move-only args)
            auto task = std::make_shared<std::packaged_task<Ret()>>(
                [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Ret {
                    return std::apply(fn, std::move(tup));
                }
            );
            std::future<Ret> fut = task->get_future();

            auto wrapper = [task]() {
                // packaged_task 会把异常转发到 future
                (*task)();
                };

            if (!EnqueueTask(std::function<void()>(wrapper))) {
                // 入队失败：返回一个已经带异常的 future
                std::promise<Ret> p;
                p.set_exception(std::make_exception_ptr(std::runtime_error("ThreadPool: Task rejected")));
                return p.get_future();
            }
            return fut;
        }
        // 提交一个任务,无返回值无参数
        bool Post(std::function<void()> fn);

        // ---- 查询与监控 ----
        size_t GetQueueSize() const;
        size_t GetActiveCount() const { return activeCount_.load(std::memory_order_acquire); } // 正在执行任务的线程数
        size_t GetThreadCount() const { return aliveThreads_.load(std::memory_order_acquire); } // 返回“活着”的线程数
        bool   IsRunning()     const { return running_.load(std::memory_order_acquire); }

        size_t IetRejectedCount() const { return rejectedCount_.load(std::memory_order_acquire); } // 被拒绝的任务数量
        size_t IetTotalTasksSubmitted() const { return submittedCount_.load(std::memory_order_acquire); }
        size_t IetCompletedCount() const { return completedCount_.load(std::memory_order_acquire); }
        size_t IetLargestPoolSize() const { return largestPoolSize_.load(std::memory_order_acquire); }
        size_t IetPeakQueueSize() const { return peakQueueSize_.load(std::memory_order_acquire); }

        // 获取快照统计信息
        Statistics Snapshot() const;

        // 等待所有线程退出（阻塞）
        void JoinAll();

    private:
        // 不可拷贝/移动
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        // 任务入队及动态扩容
        bool EnqueueTask(std::function<void()>&& task);

        // 工作线程循环体
        void WorkerLoop();

        // 创建新线程
        void SpawnWorker();

        // 唤醒所有工作线程
        void NotifyAllWorkers();

        Options opts_;

        // 真正的队列/同步结构
        mutable std::mutex queueMutex_;
        std::condition_variable queueNotEmptyCv_; // 通知 worker
        std::condition_variable queueNotFullCv_;  // 通知 submit 等待
        std::deque<std::function<void()>> taskQueue_; // 任务队列
        size_t queueCapacity_; // 队列容量

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
}
