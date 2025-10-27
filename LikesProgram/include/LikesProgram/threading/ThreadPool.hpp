#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "../time/Time.hpp"
#include "IThreadPoolObserver.hpp"
#include <functional>
#include <future>
#include <chrono>
#include <memory>
#include <utility>
#include <type_traits>

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
            Time::TimePoint lastSubmitTime{}; // 最后一次提交时间
            Time::TimePoint lastFinishTime{}; // 最后一次完成时间
            String ToString() const;
        };

        explicit ThreadPool(std::shared_ptr<IThreadPoolObserver> observer, Options opts);
        explicit ThreadPool(Options opts) : ThreadPool(nullptr, std::move(opts)) { }
        explicit ThreadPool(std::shared_ptr<IThreadPoolObserver> observer) : ThreadPool(observer, Options()) { }
        explicit ThreadPool() : ThreadPool(nullptr, Options()) { }

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

            // 调用带有移动参数的fn的packaged_task（支持仅移动参数）
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

        // 提交一个任务，无返回值，支持参数
        template<typename F, typename... Args>
        bool Post(F&& f, Args&&... args) {
            using Fn = std::decay_t<F>;

            // 打包任务，捕获函数和参数，生成一个 void() 调用
            auto task = std::make_shared<std::packaged_task<void()>>(
                [fn = Fn(std::forward<F>(f)), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                    std::apply(fn, std::move(tup));
                }
            );

            auto wrapper = [task]() {
                // packaged_task 会把异常转发到 future
                (*task)();
            };

            std::function<void(std::exception_ptr)> exceptionHandler = GetExceptionHandler();
            // 入队并返回是否成功
            bool success = EnqueueTask(std::function<void()>(wrapper));
            if (!success && exceptionHandler) exceptionHandler(std::make_exception_ptr(std::runtime_error("Task rejected")));
            return success;
        }

        // 提交一个任务,无返回值无参数
        bool PostNoArg(std::function<void()> fn);

        // ---- 查询与监控 ----
        // 获取任务队列中的任务数
        size_t GetQueueSize() const;
        // 正在执的行任务数
        size_t GetActiveCount() const;
        // 返回“活着”的线程数
        size_t GetThreadCount() const;
        // 是否在运行中
        bool   IsRunning()     const;
        // 被拒绝的任务数量
        size_t IetRejectedCount() const;
        // 总提交任务数
        size_t IetTotalTasksSubmitted() const;
        // 完成任务数
        size_t IetCompletedCount() const;
        // 历史最大线程数
        size_t IetLargestPoolSize() const;
        // 队列峰值
        size_t IetPeakQueueSize() const;

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

        // 获取异常处理函数
        std::function<void(std::exception_ptr)> GetExceptionHandler() const;

        struct ThreadPoolImpl;
        ThreadPoolImpl* m_impl = nullptr;

    public:
        static std::shared_ptr<IThreadPoolObserver> CreateDefaultThreadPoolMetrics(const String& poolName, std::shared_ptr<Metrics::Registry> registry);
    };
}
