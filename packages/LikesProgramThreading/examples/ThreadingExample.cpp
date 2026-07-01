#include <LikesProgram/Threading/Threading.hpp>
#include <iostream>

int main() {
    LikesProgram::Threading::ThreadPool::Options options(2, 4, 32);
    LikesProgram::Threading::ThreadPool pool(options);

    pool.Start();
    auto answer = pool.Submit([] {
        return 42;
    });

    std::cout << "answer=" << answer.get() << std::endl;
    pool.Shutdown();
    pool.AwaitTermination(std::chrono::seconds(5));

    auto stats = pool.Snapshot();
    std::cout << "submitted=" << stats.submitted
        << " completed=" << stats.completed << std::endl;
    return 0;
}
