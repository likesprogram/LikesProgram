#include "net/WriteQueuePolicy.hpp"

#include <iostream>
#include <stdexcept>

namespace {
    using LikesProgram::Net::Internal::WriteQueuePolicy;
    using LikesProgram::Net::Internal::WriteQueueSignal;

    // 统一把策略契约失败转换为可读的测试进程错误。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 高水位只通知一次，回落到低水位后才允许下一轮高水位通知。
    void TestWatermarkHysteresis() {
        WriteQueuePolicy policy; // 单连接独占的写队列反馈状态
        policy.ConfigureWatermark(8, 3);

        Require(policy.ObserveGrowth(7) == WriteQueueSignal::None,
            "Pending bytes below high watermark should remain quiet");
        Require(policy.ObserveGrowth(8) == WriteQueueSignal::High,
            "Pending bytes at high watermark should notify once");
        Require(policy.ObserveGrowth(12) == WriteQueueSignal::None,
            "Pending bytes above an already-notified watermark should remain quiet");
        Require(policy.ObserveDrain(4) == WriteQueueSignal::None,
            "Pending bytes above low watermark should keep backpressure latched");
        Require(policy.ObserveDrain(3) == WriteQueueSignal::Low,
            "Pending bytes at low watermark should release backpressure");
        Require(policy.ObserveGrowth(8) == WriteQueueSignal::High,
            "A low-watermark release should arm the next high notification");
    }

    // 硬上限允许精确边界，只有超过上限才触发保护性关闭信号。
    void TestHardLimitUsesStrictOverflowBoundary() {
        WriteQueuePolicy policy; // 不配置水位，只验证硬上限
        policy.ConfigureLimit(10);

        Require(policy.ObserveGrowth(10) == WriteQueueSignal::None,
            "Pending bytes at hard limit should remain accepted");
        Require(policy.ObserveGrowth(11) == WriteQueueSignal::Overflow,
            "Pending bytes above hard limit should signal overflow");
    }

    // Overflow 优先于 High，避免慢连接同时收到两种相互冲突的反馈。
    void TestOverflowPrecedesHighWatermark() {
        WriteQueuePolicy policy; // 同时配置软水位与硬上限
        policy.ConfigureWatermark(8, 4);
        policy.ConfigureLimit(10);

        Require(policy.ObserveGrowth(11) == WriteQueueSignal::Overflow,
            "Hard overflow should take precedence over high watermark");
        Require(policy.ObserveDrain(0) == WriteQueueSignal::None,
            "Overflow should not latch a high-watermark notification");
    }

    // high=0 必须关闭水位并清除旧 latch，重新配置后按新阈值工作。
    void TestDisablingWatermarkResetsLatch() {
        WriteQueuePolicy policy; // 先触发一轮高水位
        policy.ConfigureWatermark(8, 4);
        Require(policy.ObserveGrowth(8) == WriteQueueSignal::High,
            "Initial watermark should notify");

        policy.ConfigureWatermark(0, 0);
        Require(policy.ObserveDrain(0) == WriteQueueSignal::None,
            "Disabled watermark should not emit a stale low notification");
        Require(policy.ObserveGrowth(1024) == WriteQueueSignal::None,
            "Disabled watermark should ignore queue growth");

        policy.ConfigureWatermark(16, 64); // low 必须被夹到 high
        Require(policy.ObserveGrowth(16) == WriteQueueSignal::High,
            "Reconfigured watermark should arm a fresh notification");
        Require(policy.ObserveDrain(16) == WriteQueueSignal::Low,
            "Low watermark above high should clamp to the high boundary");
    }
}

int main() {
    try {
        TestWatermarkHysteresis();
        TestHardLimitUsesStrictOverflowBoundary();
        TestOverflowPrecedesHighWatermark();
        TestDisablingWatermarkResetsLatch();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; // CI 日志保留首个策略失败原因
        return 1;
    }
}
