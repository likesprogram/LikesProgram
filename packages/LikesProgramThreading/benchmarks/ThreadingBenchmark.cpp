#include <LikesProgram/Threading/Threading.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    struct Measurement {
        const char* name = "";     // 输出项名称
        double seconds = 0.0;       // 总耗时，单位秒
        double opsPerSecond = 0.0;  // 吞吐量，单位 ops/s
    };

    // 等待指定数量任务完成，避免 benchmark 把排队时间漏掉。
    void WaitForCount(const std::atomic<int64_t>& counter, int64_t expected) {
        while (counter.load(std::memory_order_acquire) < expected) {
            std::this_thread::yield();
        }
    }

    // 使用 ThreadPool 测量短任务提交与执行吞吐。
    Measurement MeasureThreadPool(int64_t tasks) {
        LikesProgram::Threading::ThreadPool::Options options(
            4, 4, static_cast<size_t>(tasks),
            LikesProgram::Threading::ThreadPool::RejectPolicy::Block);
        LikesProgram::Threading::ThreadPool pool(options);
        std::atomic<int64_t> completed{ 0 }; // 已完成任务计数

        pool.Start();
        const auto started = Clock::now();
        for (int64_t i = 0; i < tasks; ++i) {
            if (!pool.PostNoArg([&completed] {
                completed.fetch_add(1, std::memory_order_release);
            })) {
                throw std::runtime_error("ThreadPool benchmark submit failed");
            }
        }
        WaitForCount(completed, tasks);
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        pool.Shutdown();
        if (!pool.AwaitTermination(std::chrono::seconds(10))) {
            throw std::runtime_error("ThreadPool benchmark shutdown timed out");
        }
        pool.JoinAll();

        const double safeElapsed = elapsed > 0.0 ? elapsed : 1e-12;
        return Measurement{ "LikesProgram::ThreadPool", elapsed,
            static_cast<double>(tasks) / safeElapsed };
    }

    // 用标准库 mutex/cv 队列构造同级基线，比较有界队列与 worker 调度成本。
    Measurement MeasureStdQueue(int64_t tasks) {
        std::mutex mutex;                         // 保护队列和关闭标志
        std::condition_variable cv;               // 通知 worker 取任务
        std::queue<std::function<void()>> queue;  // 标准库基线任务队列
        bool done = false;                        // 是否停止 worker
        std::atomic<int64_t> completed{ 0 };      // 已完成任务数
        std::vector<std::thread> workers;         // 基线 worker 集合

        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&] {
                while (true) {
                    std::function<void()> task;   // 本次取出的任务函数
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return done || !queue.empty(); });
                        if (done && queue.empty()) return;
                        task = std::move(queue.front());
                        queue.pop();
                    }
                    task();
                }
            });
        }

        const auto started = Clock::now();
        for (int64_t i = 0; i < tasks; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                queue.push([&completed] {
                    completed.fetch_add(1, std::memory_order_release);
                });
            }
            cv.notify_one();
        }
        WaitForCount(completed, tasks);
        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }

        const double safeElapsed = elapsed > 0.0 ? elapsed : 1e-12;
        return Measurement{ "std function queue", elapsed,
            static_cast<double>(tasks) / safeElapsed };
    }

    void Print(const Measurement& value) {
        std::cout << std::left << std::setw(28) << value.name
            << " " << std::right << std::setw(12)
            << static_cast<int64_t>(value.opsPerSecond)
            << " ops/s (" << std::fixed << std::setprecision(6)
            << value.seconds << "s)" << std::endl;
    }
}

int main() {
    try {
        constexpr int64_t tasks = 100000; // Release benchmark 固定短任务规模
        const Measurement threading = MeasureThreadPool(tasks);
        const Measurement baseline = MeasureStdQueue(tasks);
        const double ratio = threading.opsPerSecond / baseline.opsPerSecond;

        Print(threading);
        Print(baseline);
        std::cout << "ThreadPool/std ratio: " << std::fixed
            << std::setprecision(4) << ratio << std::endl;

        if (threading.opsPerSecond <= 0.0 || baseline.opsPerSecond <= 0.0) return 2;
        if (ratio < 0.10) return 3;
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "LikesProgramThreadingBenchmark failed: " << ex.what() << std::endl;
        return 1;
    }
}
