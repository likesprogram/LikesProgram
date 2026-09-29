#include "net/platform/linux/IoUringPoller.hpp"
#include "net/platform/linux/ProvidedBufferPolicy.hpp"
#include "net/platform/linux/ProvidedBufferPool.hpp"
#include <LikesProgram/Net/Poller.hpp>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    template<typename PollerType>
    concept SupportsProvidedBufferReconfiguration = requires(PollerType& poller) {
        poller.ReconfigureProvidedBuffers(std::size_t{}, std::uint32_t{});
    };

    template<typename StatsType>
    concept ExposesProvidedBufferGenerations = requires(StatsType& stats) {
        stats.retiringProvidedBufferGenerations;
        stats.providedBufferReconfigurationCount;
        stats.retainedProvidedBufferGenerations;
        stats.retainedProvidedBufferBytes;
        stats.receivedBytes;
        stats.receiveCompletions;
        stats.providedBufferPolicyEvaluationCount;
        stats.providedBufferPolicyRecommendationCount;
    };

    static_assert(
        SupportsProvidedBufferReconfiguration<LikesProgram::Net::Internal::IoUringPoller>,
        "io_uring Poller should expose internal provided-buffer reconfiguration");
    static_assert(
        ExposesProvidedBufferGenerations<LikesProgram::Net::CompletionStats>,
        "Completion stats should expose provided-buffer generation transitions");

    // 统一把契约失败转换为测试进程非零退出。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void TestRetiredPoolKeepsOwnerUntilLeaseRelease() {
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(64, 4); // 模拟已注销 ring 的旧代
        if (pool == nullptr) throw std::runtime_error("Retired provided-buffer pool should be created");
        pool->RetainLease(0);
        pool->RetainLease(1);
        std::uint8_t* retained = pool->BufferAddress(0); // owner 保留期间地址必须稳定
        retained[0] = 'g';

        pool->ReleaseLease(1); // Retire 前已排队但尚未由 issuer 回填的 token
        Require(pool->PendingReturnCount() == 1,
            "Released lease should queue before its generation retires");
        pool->Retire();
        Require(pool->PendingReturnCount() == 0,
            "Retiring a generation should discard pending ring returns");
        Require(pool->CurrentAvailableCount() == 0,
            "Retired generation should expose no kernel-available buffers");
        Require(retained[0] == 'g',
            "Retired generation owner should preserve storage for outstanding leases");

        pool->ReleaseLease(0);
        Require(pool->ActiveLeaseCount() == 0,
            "Last retired lease should leave only the Poller owner reference");
        pool->Close(); // 两阶段关闭的最后一步释放 owner 并销毁存储
    }

    void TestProvidedBufferPolicyBoundsGenerationMemory() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        Require(ProvidedBufferPolicy::CanCreate(4 * 1024, 128, 4 * 1024 * 1024),
            "Policy should accept a bounded power-of-two generation");
        Require(!ProvidedBufferPolicy::CanCreate(4 * 1024, 127, 0),
            "Policy should reject a non-power-of-two buffer count");
        Require(!ProvidedBufferPolicy::CanCreate(512, 128, 0),
            "Policy should reject buffers below the completion lower bound");
        Require(!ProvidedBufferPolicy::CanCreate(64 * 1024, 1024, 1),
            "Policy should include existing generations in the 64 MiB memory cap");
        Require(!ProvidedBufferPolicy::CanCreate(
            (std::numeric_limits<std::size_t>::max)(),
            4096,
            0),
            "Policy should reject size multiplication overflow");
    }

    // 明确 ENOBUFS 时优先扩充 buffer 数量，避免继续在已耗尽的 ring 上抖动。
    void TestProvidedBufferPolicyGrowsCountAfterStarvation() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 每个 Poller 独立维护迟滞与冷却状态
        ProvidedBufferPolicy::Observation observation{}; // 当前 generation 的窗口反馈
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 128;
        observation.currentAvailable = 0;
        observation.activeLeases = 128;
        observation.enobufsDelta = 1;
        observation.receivedBytes = 8 * 1024 * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        const auto recommendation = policy.Observe(observation); // starvation 可跳过普通增长迟滞
        Require(recommendation.changed
                && recommendation.bufferSize == observation.bufferSize
                && recommendation.bufferCount == 256,
            "ENOBUFS feedback should double the provided-buffer count within bounds");

        observation.bufferCount = recommendation.bufferCount; // 模拟新 generation 已成为提交目标
        observation.existingGenerationBytes = 3 * 1024 * 1024; // 新旧代短期并存的内存峰值
        const auto cooldownRecommendation = policy.Observe(observation); // 持续饥饿也不能连续抖动切换
        Require(!cooldownRecommendation.changed,
            "Provided-buffer policy should enforce cooldown after a generation change");
    }

    // 长时间低占用才允许缩小单块尺寸，短期低负载不能触发代际切换。
    void TestProvidedBufferPolicyShrinksSizeAfterHysteresis() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 独立策略实例确保测试不继承上一用例冷却状态
        ProvidedBufferPolicy::Observation observation{}; // 低占用且平均 payload 很小
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 128;
        observation.currentAvailable = 128;
        observation.activeLeases = 0;
        observation.receivedBytes = 512 * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        for (std::uint32_t sample = 1; sample < 1024; ++sample) {
            Require(!policy.Observe(observation).changed,
                "Calm feedback should remain stable before shrink hysteresis expires");
        }
        const auto recommendation = policy.Observe(observation); // 第 1024 个稳定窗口才允许缩容
        Require(recommendation.changed
                && recommendation.bufferSize == 4 * 1024
                && recommendation.bufferCount == observation.bufferCount,
            "Sustained small receives should halve provided-buffer size within bounds");
    }

    // 新旧 generation 峰值超过总内存上界时，即使发生饥饿也必须保持当前配置。
    void TestProvidedBufferPolicyRejectsGrowthBeyondMemoryPeak() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 单次 ENOBUFS 足以提出增长，但不能越过内存闸门
        ProvidedBufferPolicy::Observation observation{}; // 当前代已占 32 MiB
        observation.bufferSize = 32 * 1024;
        observation.bufferCount = 1024;
        observation.currentAvailable = 0;
        observation.activeLeases = 1024;
        observation.enobufsDelta = 1;
        observation.receivedBytes = observation.bufferSize * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        const auto recommendation = policy.Observe(observation); // 候选 64 MiB 与旧代并存将超过 64 MiB
        Require(!recommendation.changed
                && recommendation.bufferSize == observation.bufferSize
                && recommendation.bufferCount == observation.bufferCount,
            "Provided-buffer growth should preserve the total generation memory cap");
    }

    // entry 数达到上界且持续填满单块时，扩大块尺寸以降低 completion 频率。
    void TestProvidedBufferPolicyGrowsSizeAtMaximumCount() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // count 已无增长空间，只能调整单块尺寸
        ProvidedBufferPolicy::Observation observation{}; // 平均 payload 接近当前 4 KiB 上限
        observation.bufferSize = 4 * 1024;
        observation.bufferCount = 4096;
        observation.currentAvailable = 0;
        observation.activeLeases = 4096;
        observation.enobufsDelta = 1;
        observation.receivedBytes = observation.bufferSize * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        const auto recommendation = policy.Observe(observation); // 新旧代峰值为 48 MiB
        Require(recommendation.changed
                && recommendation.bufferSize == 8 * 1024
                && recommendation.bufferCount == observation.bufferCount,
            "Maximum-count pressure should grow provided-buffer size when payloads fill each block");
    }

    // payload 已充分使用块尺寸时，长期闲置只缩 entry 数，不误缩单块容量。
    void TestProvidedBufferPolicyShrinksCountAfterHysteresis() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 独立积累低占用窗口
        ProvidedBufferPolicy::Observation observation{}; // 平均 payload 为块容量一半
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 256;
        observation.currentAvailable = 256;
        observation.activeLeases = 0;
        observation.receivedBytes = 4 * 1024 * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        for (std::uint32_t sample = 1; sample < 1024; ++sample) {
            Require(!policy.Observe(observation).changed,
                "Calm feedback should not shrink entry count before hysteresis expires");
        }
        const auto recommendation = policy.Observe(observation); // 尺寸合适时把闲置 entry 减半
        Require(recommendation.changed
                && recommendation.bufferSize == observation.bufferSize
                && recommendation.bufferCount == 128,
            "Sustained available buffers should halve entry count without shrinking payload capacity");
    }

    // retained 旧代暂时挤占内存时短退避，释放后再重新提出同一增长候选。
    void TestProvidedBufferPolicyBacksOffAfterMemoryRejection() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // 失败候选不得每个观察窗口重复尝试注册
        ProvidedBufferPolicy::Observation observation{}; // 当前代 4 MiB，旧 lease 额外保留 56 MiB
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 512;
        observation.currentAvailable = 0;
        observation.activeLeases = 512;
        observation.enobufsDelta = 1;
        observation.receivedBytes = observation.bufferSize * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = 60 * 1024 * 1024;
        Require(!policy.Observe(observation).changed,
            "Retained generation memory should reject an otherwise valid growth candidate");

        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;
        for (std::uint32_t sample = 0; sample < 16; ++sample) {
            Require(!policy.Observe(observation).changed,
                "Memory rejection should apply a short retry backoff");
        }
        const auto recommendation = policy.Observe(observation); // retained 内存释放且退避结束
        Require(recommendation.changed && recommendation.bufferCount == 1024,
            "Provided-buffer growth should resume after retained memory and backoff clear");
    }

    // CQ overflow 表示 completion 消费拥塞，不得误判为 buffer 饥饿并扩大可提交池。
    void TestProvidedBufferPolicyDoesNotGrowForCqOverflowAlone() {
        using LikesProgram::Net::Internal::ProvidedBufferPolicy;
        ProvidedBufferPolicy policy; // completion budget 独立处理 CQ 拥塞，本策略只保护 buffer 水位
        ProvidedBufferPolicy::Observation observation{}; // ring 水位充足且没有活动 lease 压力
        observation.bufferSize = 8 * 1024;
        observation.bufferCount = 128;
        observation.currentAvailable = observation.bufferCount;
        observation.activeLeases = 0;
        observation.cqOverflowDelta = 1;
        observation.receivedBytes = observation.bufferSize * 64;
        observation.receiveCompletions = 64;
        observation.existingGenerationBytes = observation.bufferSize * observation.bufferCount;

        for (std::uint32_t sample = 0; sample < 4; ++sample) {
            const auto recommendation = policy.Observe(observation); // 重复 overflow 也不能扩大 ring
            Require(!recommendation.changed
                    && recommendation.bufferSize == observation.bufferSize
                    && recommendation.bufferCount == observation.bufferCount,
                "CQ overflow alone should not resize the provided-buffer generation");
        }
    }

    // 多线程晚到 release 只能减少 lease 引用，不能重新进入已注销 ring。
    void TestRetiredPoolHandlesConcurrentLateReleases() {
        constexpr std::uint32_t kLeaseCount = 16; // 每个线程释放唯一 token
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(64, 32); // 保留 owner 的旧代
        if (pool == nullptr) throw std::runtime_error("Concurrent retired pool should be created");
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) pool->RetainLease(token);
        pool->Retire();

        std::vector<std::thread> releasers; // 模拟业务线程同时析构旧 completion Buffer
        releasers.reserve(kLeaseCount);
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) {
            releasers.emplace_back([pool, token]() { pool->ReleaseLease(token); });
        }
        for (std::thread& releaser : releasers) releaser.join();

        Require(pool->ActiveLeaseCount() == 0,
            "Concurrent retired releases should leave only the owner reference");
        Require(pool->PendingReturnCount() == 0,
            "Concurrent retired releases should not queue ring returns");
        pool->Close();
    }

    // Poller owner 先关闭后，最后一个业务线程仍必须负责安全销毁旧代存储。
    void TestClosedPoolOutlivesConcurrentLateReleases() {
        constexpr std::uint32_t kLeaseCount = 16; // Close 后仍存活的业务 lease 数
        auto* pool = LikesProgram::Net::Internal::ProvidedBufferPool::Create(64, 32); // 模拟切换中关闭 Poller
        if (pool == nullptr) throw std::runtime_error("Closing provided-buffer pool should be created");
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) pool->RetainLease(token);

        pool->Retire(); // ring 已注销，后续 token 不再允许回填
        pool->Close(); // owner 引用先释放，活动 lease 继续延长 pool
        pool->Close(); // 重复关闭不得重复释放 owner 引用

        std::vector<std::thread> releasers; // 最后一个线程会在 ReleaseLease 尾部销毁 pool
        releasers.reserve(kLeaseCount);
        for (std::uint32_t token = 0; token < kLeaseCount; ++token) {
            releasers.emplace_back([pool, token]() { pool->ReleaseLease(token); });
        }
        for (std::thread& releaser : releasers) releaser.join();
    }
}

int main() {
    try {
        TestRetiredPoolKeepsOwnerUntilLeaseRelease();
        TestProvidedBufferPolicyBoundsGenerationMemory();
        TestProvidedBufferPolicyGrowsCountAfterStarvation();
        TestProvidedBufferPolicyShrinksSizeAfterHysteresis();
        TestProvidedBufferPolicyRejectsGrowthBeyondMemoryPeak();
        TestProvidedBufferPolicyGrowsSizeAtMaximumCount();
        TestProvidedBufferPolicyShrinksCountAfterHysteresis();
        TestProvidedBufferPolicyBacksOffAfterMemoryRejection();
        TestProvidedBufferPolicyDoesNotGrowForCqOverflowAlone();
        TestRetiredPoolHandlesConcurrentLateReleases();
        TestClosedPoolOutlivesConcurrentLateReleases();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; // CI 日志直接保留首个契约失败原因
        return 1;
    }
}
