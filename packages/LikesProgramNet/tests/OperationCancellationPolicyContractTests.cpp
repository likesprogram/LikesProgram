#include "net/OperationCancellationPolicy.hpp"

#include <iostream>
#include <stdexcept>

namespace {
    using LikesProgram::Net::Internal::NeedsOperationCancellation;
    using LikesProgram::Net::Internal::RetainCancellationRequest;

    // 统一把 completion 取消策略失败转换为测试进程非零退出。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 已提交且不再需要的 operation 必须持续重试取消，直到 cancel SQE 成功入队。
    void TestUnwantedOutstandingOperationRetriesCancellation() {
        Require(NeedsOperationCancellation(true, false, false),
            "Unwanted outstanding operation should request cancellation");
        Require(!NeedsOperationCancellation(true, true, false),
            "Operation with an outstanding cancel SQE should not submit a duplicate");
        Require(!NeedsOperationCancellation(false, false, false),
            "Retired operation should not submit cancellation");
    }

    // 仍属当前状态机且需要继续运行的 operation 不得被通用 retry 误取消。
    void TestWantedOperationRemainsActive() {
        Require(!NeedsOperationCancellation(true, false, true),
            "Wanted outstanding operation should remain active");
    }

    // multishot CQE 仍带 MORE 时，先前提交的 cancel latch 必须保留到 terminal CQE。
    void TestMultishotCompletionRetainsCancellationLatch() {
        Require(RetainCancellationRequest(true, true),
            "Live multishot operation should retain its submitted cancellation");
        Require(!RetainCancellationRequest(true, false),
            "Terminal completion should clear its cancellation latch");
        Require(!RetainCancellationRequest(false, true),
            "Completion should not invent a cancellation latch");
    }
}

int main() {
    try {
        TestUnwantedOutstandingOperationRetriesCancellation();
        TestWantedOperationRemainsActive();
        TestMultishotCompletionRetainsCancellationLatch();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; // CI 日志保留首个策略失败原因
        return 1;
    }
}
