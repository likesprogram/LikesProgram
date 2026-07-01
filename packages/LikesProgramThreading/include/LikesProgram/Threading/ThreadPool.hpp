#pragma once
#include <LikesProgram/Threading/ThreadPoolObserver.hpp>
#include <LikesProgram/Core/String.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace LikesProgram {
    namespace Threading {
        // 任务队列满时的处理策略。
        enum class ThreadPoolRejectPolicy {
            Block,
            Discard,
            DiscardOld,
            Throw
        };

        // 关闭时队列处理策略。
        enum class ThreadPoolShutdownPolicy {
            Graceful,
            Drain,
            CancelNow
        };

        // 线程池配置选项；保持头内实现，避免跨 DLL 导出 STL/chrono 字段。
        struct ThreadPoolOptions {
            size_t coreThreads = 0;           // 最少工作线程数，0 表示按硬件并发推导
            size_t maxThreads = 0;            // 最大工作线程数，0 表示按 coreThreads 推导
            size_t queueCapacity = 1024;      // 队列容量，0 会被规整为 1
            ThreadPoolRejectPolicy rejectPolicy = ThreadPoolRejectPolicy::Block; // 队列满时处理策略
            std::chrono::milliseconds keepAlive{ 30000 };     // 动态线程空闲回收时间
            bool allowDynamicResize = true;   // 是否允许超过 coreThreads 动态扩容
            String threadNamePrefix = u"tp-worker-"; // 工作线程展示名前缀，平台不支持时忽略
            std::function<void(std::exception_ptr)> exceptionHandler =
                [](std::exception_ptr) {};     // 任务或回调异常处理器

            // 构造线程池配置，并规整线程数默认值。
            ThreadPoolOptions(size_t coreThreads = 0, size_t maxThreads = 0, size_t queueCapacity = 1024,
                ThreadPoolRejectPolicy rejectPolicy = ThreadPoolRejectPolicy::Block,
                std::chrono::milliseconds keepAlive = std::chrono::milliseconds(30000),
                bool allowDynamicResize = true, String threadNamePrefix = u"tp-worker-",
                std::function<void(std::exception_ptr)> exceptionHandler = [](std::exception_ptr) {})
                : coreThreads(coreThreads ? coreThreads :
                    (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)),
                maxThreads(maxThreads ? maxThreads :
                    (coreThreads ? coreThreads :
                        (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1))),
                queueCapacity(queueCapacity > 0 ? queueCapacity : 1),
                rejectPolicy(rejectPolicy),
                keepAlive(keepAlive.count() > 0 ? keepAlive : std::chrono::milliseconds(1)),
                allowDynamicResize(allowDynamicResize),
                threadNamePrefix(std::move(threadNamePrefix)),
                exceptionHandler(std::move(exceptionHandler)) {
                // 最大线程数不能小于核心线程数，避免动态扩容边界反转。
                if (this->maxThreads < this->coreThreads) this->maxThreads = this->coreThreads;
            }
        };

        // 运行时统计快照；不作为导出类型，避免 C4251 污染公共头。
        struct ThreadPoolStatistics {
            size_t submitted = 0;       // 成功提交任务数
            size_t rejected = 0;        // 拒绝或取消任务数
            size_t completed = 0;       // 完成任务数
            size_t active = 0;          // 正在执行任务数
            size_t aliveThreads = 0;    // 存活工作线程数
            size_t largestPoolSize = 0; // 历史最大线程数
            size_t peakQueueSize = 0;   // 历史最大队列长度
            Time::TimePoint lastSubmitTime{}; // 最近一次提交时间
            Time::TimePoint lastFinishTime{}; // 最近一次完成时间

            // 生成可读统计文本。
            String ToString() const {
                String stats;
                // 输出保持中文短说明，便于直接写入诊断日志。
                stats.Append(String(u"成功入队的任务数：")).Append(String(std::to_string(submitted))).Append(String(u"\r\n"));
                stats.Append(String(u"被拒绝的任务数：")).Append(String(std::to_string(rejected))).Append(String(u"\r\n"));
                stats.Append(String(u"执行完成的任务数：")).Append(String(std::to_string(completed))).Append(String(u"\r\n"));
                stats.Append(String(u"正在执行的任务数：")).Append(String(std::to_string(active))).Append(String(u"\r\n"));
                stats.Append(String(u"存活工作线程数：")).Append(String(std::to_string(aliveThreads))).Append(String(u"\r\n"));
                stats.Append(String(u"历史最大线程数：")).Append(String(std::to_string(largestPoolSize))).Append(String(u"\r\n"));
                stats.Append(String(u"队列峰值：")).Append(String(std::to_string(peakQueueSize))).Append(String(u"\r\n"));
                stats.Append(String(u"最后一次提交时间："))
                    .Append(Time::FormatTime(lastSubmitTime, u"%Y-%m-%d %H:%M:%S.%f")).Append(String(u"\r\n"));
                stats.Append(String(u"最后一次完成时间："))
                    .Append(Time::FormatTime(lastFinishTime, u"%Y-%m-%d %H:%M:%S.%f")).Append(String(u"\r\n"));
                return stats;
            }
        };

        // 可独立使用的线程池，支持有界队列、动态扩缩容、观察者事件和关闭策略。
        class LIKESPROGRAM_THREADING_API ThreadPool {
        public:
            using RejectPolicy = ThreadPoolRejectPolicy;
            using ShutdownPolicy = ThreadPoolShutdownPolicy;
            using Options = ThreadPoolOptions;
            using Statistics = ThreadPoolStatistics;

            // 使用观察者和配置创建线程池。
            explicit ThreadPool(std::shared_ptr<IThreadPoolObserver> observer, Options options);

            // 使用配置创建无观察者线程池。
            explicit ThreadPool(Options options);

            // 使用观察者创建默认配置线程池。
            explicit ThreadPool(std::shared_ptr<IThreadPoolObserver> observer);

            // 创建默认线程池。
            ThreadPool();

            // 关闭并释放所有 worker。
            ~ThreadPool();

            // 启动线程池，重复调用保持幂等。
            void Start();

            // 按指定策略关闭线程池。
            void Shutdown(ShutdownPolicy mode = ShutdownPolicy::Graceful);

            // 兼容旧接口：优雅关闭。
            void Stop() { Shutdown(ShutdownPolicy::Graceful); }

            // 兼容旧接口：取消等待队列并尽快关闭。
            void ShutdownNow() { Shutdown(ShutdownPolicy::CancelNow); }

            // 等待 worker 退出，timeout=0 表示非阻塞检查。
            bool AwaitTermination(std::chrono::milliseconds timeout);

            // 提交有返回值任务。
            template<typename F, typename... Args>
            auto Submit(F&& function, Args&&... args)
                -> std::future<std::invoke_result_t<F, Args...>> {
                using Ret = std::invoke_result_t<F, Args...>;

                auto task = std::make_shared<std::packaged_task<Ret()>>(
                    [fn = std::forward<F>(function),
                    tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Ret {
                        return std::apply(std::move(fn), std::move(tup));
                    });
                std::future<Ret> future = task->get_future(); // 调用方持有的结果通道

                auto wrapper = [task]() {
                    // packaged_task 会把任务异常转发给 future。
                    (*task)();
                };

                if (!EnqueueTask(std::function<void()>(wrapper))) {
                    std::promise<Ret> promise; // 拒绝时返回已失败 future
                    promise.set_exception(std::make_exception_ptr(std::runtime_error("ThreadPool: Task rejected")));
                    return promise.get_future();
                }
                return future;
            }

            // 提交无返回值任务，返回是否成功入队。
            template<typename F, typename... Args>
            bool Post(F&& function, Args&&... args) {
                using Fn = std::decay_t<F>;

                auto taskState = std::make_shared<std::tuple<Fn, std::decay_t<Args>...>>(
                    Fn(std::forward<F>(function)), std::forward<Args>(args)...);

                auto wrapper = [taskState]() mutable {
                    // Post 没有 future 返回通道，任务异常必须交给 worker 的异常隔离路径处理。
                    std::apply([](auto& fn, auto&... boundArgs) {
                        std::invoke(std::move(fn), std::move(boundArgs)...);
                    }, *taskState);
                };

                bool success = EnqueueTask(std::function<void()>(wrapper)); // 入队结果
                if (!success) ReportException(std::make_exception_ptr(std::runtime_error("Task rejected")));
                return success;
            }

            // 提交无参数 void 任务。
            bool PostNoArg(std::function<void()> function);

            // 返回当前队列长度。
            size_t GetQueueSize() const;

            // 返回正在执行任务数。
            size_t GetActiveCount() const;

            // 返回存活工作线程数。
            size_t GetThreadCount() const;

            // 返回线程池是否仍接受任务。
            bool IsRunning() const;

            // 返回被拒绝任务数，保留旧拼写兼容。
            size_t IetRejectedCount() const;

            // 返回成功提交任务数，保留旧拼写兼容。
            size_t IetTotalTasksSubmitted() const;

            // 返回完成任务数，保留旧拼写兼容。
            size_t IetCompletedCount() const;

            // 返回历史最大线程数，保留旧拼写兼容。
            size_t IetLargestPoolSize() const;

            // 返回历史最大队列长度，保留旧拼写兼容。
            size_t IetPeakQueueSize() const;

            // 返回统计快照。
            Statistics Snapshot() const;

            // 等待并清理所有 worker。
            void JoinAll();

        private:
            ThreadPool(const ThreadPool&) = delete;
            ThreadPool& operator=(const ThreadPool&) = delete;
            ThreadPool(ThreadPool&&) noexcept = delete;
            ThreadPool& operator=(ThreadPool&&) noexcept = delete;

            // 尝试入队任务，并按拒绝策略处理满队列。
            bool EnqueueTask(std::function<void()>&& task);

            // worker 主循环。
            void WorkerLoop();

            // 尝试创建一个 worker，返回是否实际新增线程。
            bool SpawnWorker();

            // 唤醒所有等待中的 worker 和提交者。
            void NotifyAllWorkers();

            // 获取异常处理器快照。
            std::function<void(std::exception_ptr)> GetExceptionHandler() const;

            // 上报异常并隔离异常处理器自身异常。
            void ReportException(std::exception_ptr error) const;

            // 构造观察者事件快照。
            ThreadPoolEvent MakeEvent(Time::Nanoseconds duration = Time::Nanoseconds{ 0 }) const;

            // 使用已知队列长度构造事件，避免持有队列锁时重复加锁。
            ThreadPoolEvent MakeEventWithQueueSize(size_t queueSize,
                Time::Nanoseconds duration = Time::Nanoseconds{ 0 }) const;

            // 安全调用观察者，避免观察者异常杀死 worker。
            void NotifyTaskSubmitted(const ThreadPoolEvent& event);
            void NotifyTaskRejected(const ThreadPoolEvent& event);
            void NotifyTaskStarted(const ThreadPoolEvent& event);
            void NotifyTaskCompleted(const ThreadPoolEvent& event);
            void NotifyThreadCountAdded(const ThreadPoolEvent& event);
            void NotifyThreadCountRemoved(const ThreadPoolEvent& event);

            struct ThreadPoolImpl;
            ThreadPoolImpl* m_impl = nullptr; // 唯一拥有的线程池运行状态
        };
    }

    using ThreadPool = Threading::ThreadPool;
}
