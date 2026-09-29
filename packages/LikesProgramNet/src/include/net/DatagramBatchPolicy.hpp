#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            // 收敛内核长度、buffer 容量和批量配置后的数据报交付结果。
            struct DatagramBatchReceiveResult {
                std::size_t payloadBytes = 0;  // 实际交付给业务的容量内字节数
                std::size_t originalBytes = 0; // 内核报告的原始数据报长度
                bool truncated = false;        // 数据报是否超过任一接收上限
            };

            // 汇总控制区、peer 地址区和 payload 区的单块注册容量。
            constexpr std::size_t DatagramProvidedBufferSize(
                std::size_t controlBytes,
                std::size_t peerBytes,
                std::size_t payloadBytes) noexcept {
                return controlBytes + peerBytes + payloadBytes;
            }

            // 安全计算 UDP pool 总字节数；失败时输出归零且不执行溢出乘法。
            constexpr bool TryMultiplyDatagramBufferBytes(
                std::size_t bufferSize,
                std::uint32_t bufferCount,
                std::size_t& bytes) noexcept {
                bytes = 0;
                if (bufferSize == 0 || bufferCount == 0) return true;
                if (bufferSize > (std::numeric_limits<std::size_t>::max)() / bufferCount) {
                    return false;
                }
                bytes = bufferSize * bufferCount;
                return true;
            }

            // 按 ring 深度渐进扩容，同时保持 64 到 256 的保守边界。
            constexpr std::uint32_t DatagramProvidedBufferCount(
                unsigned int ringEntries) noexcept {
                std::uint32_t count = 64;                 // provided-buffer 最小注册数量
                const std::uint32_t target = ringEntries / 16U; // ring 深度对应的目标水位
                while (count < target && count < 256U) count *= 2U;
                return count;
            }

            // 普通重试立即允许；ENOBUFS 等待态必须先观察到至少一个可用 UDP token。
            constexpr bool ShouldRetryDatagramRead(
                bool waitingForProvidedBuffer,
                std::size_t availableBufferCount) noexcept {
                return !waitingForProvidedBuffer || availableBufferCount != 0;
            }

            // 发送批次同时受排队量、SQ 空间和固定单批上限约束。
            constexpr std::size_t DatagramSendBatchSize(
                std::size_t queued,
                std::size_t sqSpace,
                bool outstanding) noexcept {
                if (outstanding) return 0;
                const std::size_t available = queued < sqSpace ? queued : sqSpace; // 当前可提交数量
                return available < 16 ? available : 16;
            }

            // 以 buffer 与配置的较小上限解释 multishot 数据报完成结果。
            constexpr DatagramBatchReceiveResult InterpretDatagramBatchReceive(
                std::size_t reportedBytes,
                std::size_t availableBytes,
                std::size_t configuredBytes,
                bool truncatedFlag) noexcept {
                const std::size_t bufferLimited =
                    reportedBytes < availableBytes ? reportedBytes : availableBytes; // buffer 容量内长度
                const std::size_t payloadBytes =
                    bufferLimited < configuredBytes ? bufferLimited : configuredBytes; // 最终业务长度
                return {
                    payloadBytes,
                    reportedBytes,
                    truncatedFlag || reportedBytes > availableBytes || reportedBytes > configuredBytes
                };
            }
        }
    }
}
