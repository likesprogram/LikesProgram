#include <LikesProgram/Threading/Threading.hpp>
#include <atomic>
#include <chrono>
#include <iostream>

int main() {
    if (!LikesProgram::Threading::PackageAvailable()) return 1;

    std::atomic<int> posted{ 0 }; // Post 任务完成计数
    LikesProgram::Threading::ThreadPool::Options options(2, 2, 16);
    LikesProgram::Threading::ThreadPool pool(options);
    pool.Start();

    auto value = pool.Submit([] { return 42; }); // 验证 future 返回值路径
    for (int i = 0; i < 4; ++i) {
        if (!pool.Post([&posted] {
            posted.fetch_add(1, std::memory_order_relaxed);
        })) {
            return 2;
        }
    }

    if (value.get() != 42) return 3;
    pool.Shutdown();
    if (!pool.AwaitTermination(std::chrono::seconds(5))) return 4;
    pool.JoinAll();

    const auto stats = pool.Snapshot(); // 关闭后的稳定统计快照
    if (posted.load(std::memory_order_relaxed) != 4) return 5;
    if (stats.submitted != 5 || stats.completed != 5 || stats.active != 0) return 6;

    std::cout << LikesProgram::Threading::PackageName()
        << " consumer check passed\n";
    return 0;
}
