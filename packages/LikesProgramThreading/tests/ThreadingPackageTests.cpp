#include <LikesProgram/Threading/Threading.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    class CountingObserver : public LikesProgram::Threading::ThreadPoolObserverBase {
    public:
        void OnTaskSubmitted(const LikesProgram::Threading::ThreadPoolEvent& event) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_submitted;
            m_lastQueueSize = event.queueSize;
        }

        void OnTaskRejected(const LikesProgram::Threading::ThreadPoolEvent&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_rejected;
        }

        void OnTaskStarted(const LikesProgram::Threading::ThreadPoolEvent&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_started;
        }

        void OnTaskCompleted(const LikesProgram::Threading::ThreadPoolEvent& event) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_completed;
            m_lastDuration = event.duration;
        }

        void OnThreadCountAdded(const LikesProgram::Threading::ThreadPoolEvent&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_threadsAdded;
        }

        void OnThreadCountRemoved(const LikesProgram::Threading::ThreadPoolEvent&) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_threadsRemoved;
        }

        size_t Submitted() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_submitted;
        }

        size_t Rejected() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_rejected;
        }

        size_t Started() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_started;
        }

        size_t Completed() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_completed;
        }

        size_t ThreadsAdded() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_threadsAdded;
        }

        size_t ThreadsRemoved() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_threadsRemoved;
        }

        LikesProgram::Time::Nanoseconds LastDuration() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lastDuration;
        }

    private:
        mutable std::mutex m_mutex; // 保护测试观察者计数，worker 和断言线程并发访问
        size_t m_submitted = 0;     // 已收到的提交事件数
        size_t m_rejected = 0;      // 已收到的拒绝事件数
        size_t m_started = 0;       // 已收到的开始事件数
        size_t m_completed = 0;     // 已收到的完成事件数
        size_t m_threadsAdded = 0;  // 已收到的线程新增事件数
        size_t m_threadsRemoved = 0;// 已收到的线程退出事件数
        size_t m_lastQueueSize = 0; // 最近一次提交事件队列长度
        LikesProgram::Time::Nanoseconds m_lastDuration{ 0 }; // 最近一次任务耗时
    };

    class ThrowingObserver : public LikesProgram::Threading::ThreadPoolObserverBase {
    public:
        void OnTaskSubmitted(const LikesProgram::Threading::ThreadPoolEvent&) override {
            throw std::runtime_error("observer failure");
        }
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Threading::PackageName();
        const char* packageVersion = LikesProgram::Threading::PackageVersion();

        Require(LikesProgram::Threading::PackageAvailable(), "Threading package should be available");
        Require(std::strcmp(packageName, "LikesProgramThreading") == 0, "Threading package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Threading package version should follow Core version");
        LikesProgram::ThreadPool poolAlias; // 顶层短别名应指向 Threading::ThreadPool
        Require(!poolAlias.IsRunning(), "Default ThreadPool alias should not auto-start");
    }

    void TestSubmitAndStatistics() {
        auto observer = std::make_shared<CountingObserver>();
        LikesProgram::Threading::ThreadPool::Options options(1, 2, 8);
        LikesProgram::Threading::ThreadPool pool(observer, options);

        pool.Start();
        auto future = pool.Submit([](int left, int right) {
            return left + right;
        }, 20, 22);
        Require(future.get() == 42, "Submit should return task result");
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "ThreadPool should terminate");
        pool.JoinAll();

        auto stats = pool.Snapshot();
        Require(stats.submitted == 1, "Statistics should count submitted task");
        Require(stats.completed == 1, "Statistics should count completed task");
        Require(observer->Submitted() == 1, "Observer should see submitted event");
        Require(observer->Started() == 1, "Observer should see started event");
        Require(observer->Completed() == 1, "Observer should see completed event");
        Require(observer->ThreadsAdded() >= 1, "Observer should see worker creation");
        Require(observer->ThreadsRemoved() >= 1, "Observer should see worker removal");
        Require(observer->LastDuration().count() >= 0, "Completed event should carry duration");
    }

    void TestRejectPolicyDiscard() {
        auto observer = std::make_shared<CountingObserver>();
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 1,
            LikesProgram::Threading::ThreadPool::RejectPolicy::Discard);
        LikesProgram::Threading::ThreadPool pool(observer, options);

        std::atomic<bool> release{ false }; // 控制首个任务占住 worker
        pool.Start();
        Require(pool.Post([&release] {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }), "First task should be accepted");

        size_t rejected = 0; // 记录 Discard 策略拒绝次数
        for (int i = 0; i < 256; ++i) {
            if (!pool.Post([] {})) {
                ++rejected;
                break;
            }
        }

        release.store(true, std::memory_order_release);
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "Discard test pool should terminate");
        pool.JoinAll();

        Require(rejected > 0, "Discard policy should reject when queue is full");
        Require(pool.IetRejectedCount() > 0, "Rejected count should increase");
        Require(observer->Rejected() > 0, "Observer should see rejection");
    }

    void TestObserverExceptionIsolation() {
        std::atomic<size_t> exceptions{ 0 }; // 异常处理器调用次数
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 4);
        options.exceptionHandler = [&exceptions](std::exception_ptr) {
            exceptions.fetch_add(1, std::memory_order_relaxed);
        };
        LikesProgram::Threading::ThreadPool pool(std::make_shared<ThrowingObserver>(), options);

        pool.Start();
        Require(pool.Post([] {}), "Task should still be accepted with throwing observer");
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "Throwing observer pool should terminate");
        pool.JoinAll();

        Require(pool.IetCompletedCount() == 1, "Observer exception should not stop task execution");
        Require(exceptions.load(std::memory_order_relaxed) >= 1,
            "Observer exception should be routed to exception handler");
    }

    void TestShutdownNowCancelsQueue() {
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 16);
        LikesProgram::Threading::ThreadPool pool(options);

        std::atomic<bool> release{ false }; // 阻塞首个任务，制造等待队列
        pool.Start();
        Require(pool.Post([&release] {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }), "Blocking task should be accepted");
        for (int i = 0; i < 8; ++i) {
            pool.Post([] {});
        }

        pool.ShutdownNow();
        release.store(true, std::memory_order_release);
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "ShutdownNow pool should terminate");
        pool.JoinAll();
        Require(pool.IetRejectedCount() > 0, "ShutdownNow should count canceled queued tasks");
    }

    void TestPostExceptionIsReported() {
        std::atomic<size_t> exceptions{ 0 }; // Post 异常应进入隔离处理器
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 4);
        options.exceptionHandler = [&exceptions](std::exception_ptr) {
            exceptions.fetch_add(1, std::memory_order_relaxed);
        };
        LikesProgram::Threading::ThreadPool pool(options);

        pool.Start();
        Require(pool.Post([] {
            throw std::runtime_error("post failure");
        }), "Throwing Post task should be accepted");
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "Post exception pool should terminate");
        pool.JoinAll();

        Require(pool.IetCompletedCount() == 1, "Throwing Post task should still count as completed");
        Require(exceptions.load(std::memory_order_relaxed) == 1,
            "Post task exception should be routed to exception handler exactly once");
    }

    void TestSubmitExceptionFuture() {
        std::atomic<size_t> exceptions{ 0 }; // Submit 异常只应进入 future
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 4);
        options.exceptionHandler = [&exceptions](std::exception_ptr) {
            exceptions.fetch_add(1, std::memory_order_relaxed);
        };
        LikesProgram::Threading::ThreadPool pool(options);

        pool.Start();
        auto future = pool.Submit([]() -> int {
            throw std::runtime_error("submit failure");
        });
        bool threw = false; // future 是否收到任务异常
        try {
            (void)future.get();
        }
        catch (const std::runtime_error&) {
            threw = true;
        }
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "Submit exception pool should terminate");
        pool.JoinAll();

        Require(threw, "Submit task exception should be observed through future");
        Require(exceptions.load(std::memory_order_relaxed) == 0,
            "Submit packaged_task exception should not be double-reported");
    }

    void TestConcurrentSubmitAndMaxThreads() {
        auto observer = std::make_shared<CountingObserver>();
        LikesProgram::Threading::ThreadPool::Options options(2, 4, 4096);
        LikesProgram::Threading::ThreadPool pool(observer, options);
        std::atomic<size_t> completed{ 0 }; // 并发提交完成计数
        std::vector<std::thread> submitters; // 外部提交线程集合

        pool.Start();
        for (size_t t = 0; t < 8; ++t) {
            submitters.emplace_back([&pool, &completed] {
                for (size_t i = 0; i < 250; ++i) {
                    if (!pool.Post([&completed] {
                        completed.fetch_add(1, std::memory_order_relaxed);
                    })) {
                        throw std::runtime_error("Concurrent Post should not be rejected");
                    }
                }
            });
        }

        for (auto& submitter : submitters) {
            submitter.join();
        }
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(10)), "Concurrent submit pool should terminate");
        pool.JoinAll();

        Require(completed.load(std::memory_order_relaxed) == 2000,
            "Concurrent submitted tasks should all complete");
        Require(pool.IetTotalTasksSubmitted() == 2000,
            "Concurrent submit should count all accepted tasks");
        Require(pool.IetLargestPoolSize() <= 4, "Dynamic resize should not exceed maxThreads");
        Require(observer->Completed() == 2000, "Observer should see all completed concurrent tasks");
    }

    void TestDiscardOldPolicy() {
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 2,
            LikesProgram::Threading::ThreadPool::RejectPolicy::DiscardOld);
        LikesProgram::Threading::ThreadPool pool(options);
        std::atomic<bool> release{ false }; // 阻塞 worker，让队列可稳定填满
        std::atomic<bool> blockingStarted{ false }; // 确认阻塞任务已占住 worker
        std::mutex mutex;                   // 保护执行顺序记录
        std::vector<int> executed;           // 实际执行的任务编号

        pool.Start();
        Require(pool.Post([&release, &blockingStarted] {
            blockingStarted.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }), "Blocking task should be accepted");
        while (!blockingStarted.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        for (int value = 1; value <= 4; ++value) {
            Require(pool.Post([&mutex, &executed, value] {
                std::lock_guard<std::mutex> lock(mutex);
                executed.push_back(value);
            }), "DiscardOld should accept newest task after dropping oldest");
        }

        release.store(true, std::memory_order_release);
        pool.Shutdown();
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "DiscardOld pool should terminate");
        pool.JoinAll();

        std::lock_guard<std::mutex> lock(mutex);
        Require(pool.IetRejectedCount() >= 2, "DiscardOld should count dropped old tasks");
        Require(executed.size() == 2, "DiscardOld queue capacity should leave only newest pending tasks");
        Require(executed[0] == 3 && executed[1] == 4,
            "DiscardOld should preserve newest queued tasks");
    }

    void TestBlockPolicyUnblocksOnShutdown() {
        LikesProgram::Threading::ThreadPool::Options options(1, 1, 1,
            LikesProgram::Threading::ThreadPool::RejectPolicy::Block);
        LikesProgram::Threading::ThreadPool pool(options);
        std::atomic<bool> release{ false }; // 控制 worker 不释放队列空位
        std::atomic<bool> blockingStarted{ false }; // 确认 worker 已被首任务占用

        pool.Start();
        Require(pool.Post([&release, &blockingStarted] {
            blockingStarted.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }), "Blocking task should be accepted");
        while (!blockingStarted.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(pool.Post([] {}), "Queued task should fill capacity");

        auto blockedSubmit = std::async(std::launch::async, [&pool] {
            return pool.Post([] {});
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pool.Shutdown();
        release.store(true, std::memory_order_release);

        Require(blockedSubmit.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Blocked submitter should wake when pool shuts down");
        Require(!blockedSubmit.get(), "Blocked submitter should report rejection after shutdown");
        Require(pool.AwaitTermination(std::chrono::seconds(5)), "Block policy pool should terminate");
        pool.JoinAll();
    }

    void TestRepeatedStartStopAndSnapshot() {
        LikesProgram::Threading::ThreadPool::Options options(1, 2, 8);
        LikesProgram::Threading::ThreadPool pool(options);

        for (int round = 0; round < 3; ++round) {
            std::atomic<size_t> completed{ 0 }; // 当前轮完成数
            pool.Start();
            for (int i = 0; i < 10; ++i) {
                Require(pool.Post([&completed] {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }), "Repeated Start/Post should accept tasks");
            }
            pool.Shutdown();
            Require(pool.AwaitTermination(std::chrono::seconds(5)), "Repeated pool should terminate");
            pool.JoinAll();
            Require(completed.load(std::memory_order_relaxed) == 10,
                "Repeated Start/Stop round should complete all tasks");
        }

        const auto stats = pool.Snapshot(); // 多轮统计应保留累计值
        Require(stats.submitted == 30 && stats.completed == 30,
            "Snapshot should retain cumulative submitted/completed values");
        Require(!stats.ToString().Empty(), "Snapshot ToString should produce diagnostics text");
    }
}

int main() {
    try {
        TestPackageIdentity();
        TestSubmitAndStatistics();
        TestRejectPolicyDiscard();
        TestObserverExceptionIsolation();
        TestShutdownNowCancelsQueue();
        TestPostExceptionIsReported();
        TestSubmitExceptionFuture();
        TestConcurrentSubmitAndMaxThreads();
        TestDiscardOldPolicy();
        TestBlockPolicyUnblocksOnShutdown();
        TestRepeatedStartStopAndSnapshot();
    }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
