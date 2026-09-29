#include "net/platform/linux/ProvidedBufferPolicy.hpp"

#include <algorithm>
#include <limits>

namespace {
    constexpr std::size_t kMinimumProvidedBufferSize = 1024; // 避免过小 buffer 放大 completion 数
    constexpr std::size_t kMaximumProvidedBufferSize = 64 * 1024; // 单 completion 连续内存上界
    constexpr std::uint32_t kMinimumProvidedBufferCount = 64; // 低并发也保留基本池水位
    constexpr std::uint32_t kMaximumProvidedBufferCount = 4096; // 与单 loop SQ 上界保持同量级
    constexpr std::size_t kMaximumProvidedBufferMemory = 64 * 1024 * 1024; // 新旧 generation 总峰值
    constexpr std::uint32_t kGrowthHysteresisSamples = 2; // 非饥饿压力连续出现才允许扩容
    constexpr std::uint32_t kShrinkHysteresisSamples = 1024; // 长期低占用后才允许缩容
    constexpr std::uint32_t kReconfigurationCooldownSamples = 256; // generation 切换后的防抖窗口

    // buffer ring entry 数必须是二次幂，才能安全使用 liburing mask。
    bool IsPowerOfTwo(std::uint32_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool HasValidDimensions(std::size_t bufferSize, std::uint32_t bufferCount) noexcept {
        return bufferSize >= kMinimumProvidedBufferSize
            && bufferSize <= kMaximumProvidedBufferSize
            && bufferCount >= kMinimumProvidedBufferCount
            && bufferCount <= kMaximumProvidedBufferCount
            && IsPowerOfTwo(bufferCount);
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            bool ProvidedBufferPolicy::CanCreate(
                std::size_t bufferSize,
                std::uint32_t bufferCount,
                std::size_t existingGenerationBytes) noexcept {
                if (!HasValidDimensions(bufferSize, bufferCount)) return false;
                if (bufferSize > (std::numeric_limits<std::size_t>::max)() / bufferCount) return false;

                const std::size_t generationBytes = bufferSize * bufferCount; // 乘法已完成溢出检查
                if (existingGenerationBytes > kMaximumProvidedBufferMemory) return false;
                return generationBytes <= kMaximumProvidedBufferMemory - existingGenerationBytes;
            }

            ProvidedBufferPolicy::Recommendation ProvidedBufferPolicy::Observe(
                const Observation& observation) noexcept {
                Recommendation result{
                    observation.bufferSize,
                    observation.bufferCount,
                    false
                }; // 默认保持当前 generation
                if (!HasValidDimensions(observation.bufferSize, observation.bufferCount)) return result;

                if (m_cooldownSamples > 0) {
                    --m_cooldownSamples; // 冷却期只积累新统计，不允许连续 cancel 全部 read
                    m_pressureSamples = 0;
                    m_calmSamples = 0;
                    return result;
                }

                const std::size_t lowWatermark = std::max<std::size_t>(
                    1,
                    observation.bufferCount / 16); // 低于 6.25% 视为 ring 压力
                const bool bufferStarved = observation.enobufsDelta > 0
                    || observation.currentAvailable <= lowWatermark; // 只有 buffer 压力允许扩大 generation
                const bool completionCongested = observation.cqOverflowDelta > 0; // CQ 拥塞交给 completion budget
                const bool calm = !bufferStarved
                    && !completionCongested
                    && observation.receiveCompletions > 0
                    && observation.activeLeases <= lowWatermark
                    && observation.currentAvailable
                        >= observation.bufferCount - observation.bufferCount / 8;
                const std::uint64_t averageReceiveBytes = observation.receiveCompletions != 0
                    ? observation.receivedBytes / observation.receiveCompletions
                    : 0; // 当前窗口平均 payload，用于判断单块尺寸是否过大或过小

                if (bufferStarved) {
                    m_calmSamples = 0;
                    m_pressureSamples = observation.enobufsDelta > 0
                        ? kGrowthHysteresisSamples
                        : std::min(kGrowthHysteresisSamples, m_pressureSamples + 1);
                    if (m_pressureSamples < kGrowthHysteresisSamples) return result;

                    if (observation.bufferCount < kMaximumProvidedBufferCount) {
                        result.bufferCount = std::min(
                            kMaximumProvidedBufferCount,
                            observation.bufferCount * 2); // buffer 饥饿优先增加可并行 lease 数
                    }
                    else if (averageReceiveBytes >= observation.bufferSize * 3 / 4
                        && observation.bufferSize < kMaximumProvidedBufferSize) {
                        result.bufferSize = std::min(
                            kMaximumProvidedBufferSize,
                            observation.bufferSize * 2); // count 到顶后才用更大块降低完成频率
                    }
                }
                else if (completionCongested) {
                    // CQ overflow 只说明消费侧拥塞；扩 ring 会放大继续产生 completion 的窗口。
                    m_pressureSamples = 0;
                    m_calmSamples = 0;
                    return result;
                }
                else if (calm) {
                    m_pressureSamples = 0;
                    m_calmSamples = std::min(kShrinkHysteresisSamples, m_calmSamples + 1);
                    if (m_calmSamples < kShrinkHysteresisSamples) return result;

                    if (averageReceiveBytes > 0
                        && averageReceiveBytes <= observation.bufferSize / 4
                        && observation.bufferSize > kMinimumProvidedBufferSize) {
                        result.bufferSize = std::max(
                            kMinimumProvidedBufferSize,
                            observation.bufferSize / 2); // 小 payload 长稳后减少单 buffer 内存浪费
                    }
                    else if (observation.bufferCount > kMinimumProvidedBufferCount) {
                        result.bufferCount = std::max(
                            kMinimumProvidedBufferCount,
                            observation.bufferCount / 2); // 尺寸匹配时再收缩闲置 entry 数
                    }
                }
                else {
                    m_pressureSamples = 0; // 中间负载区不延续旧压力或低占用趋势
                    m_calmSamples = 0;
                    return result;
                }

                if ((result.bufferSize == observation.bufferSize
                        && result.bufferCount == observation.bufferCount)
                    || !CanCreate(
                        result.bufferSize,
                        result.bufferCount,
                        observation.existingGenerationBytes)) {
                    result.bufferSize = observation.bufferSize;
                    result.bufferCount = observation.bufferCount;
                    m_cooldownSamples = 16; // 内存峰值暂不可用时短暂退避后重看 retained 释放
                    return result;
                }

                result.changed = true;
                m_pressureSamples = 0;
                m_calmSamples = 0;
                m_cooldownSamples = kReconfigurationCooldownSamples;
                return result;
            }
        }
    }
}
