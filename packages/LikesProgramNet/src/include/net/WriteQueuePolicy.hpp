#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class WriteQueueSignal {
                None,
                High,
                Low,
                Overflow
            };

            class WriteQueuePolicy final {
            public:
                // 配置软水位；high=0 时禁用并清除旧通知 latch。
                void ConfigureWatermark(
                    std::size_t highWatermarkBytes,
                    std::size_t lowWatermarkBytes) noexcept {
                    if (highWatermarkBytes == 0) {
                        m_highWatermarkBytes.store(0, std::memory_order_release);
                        m_lowWatermarkBytes.store(0, std::memory_order_release);
                        m_highNotified.store(false, std::memory_order_release);
                        return;
                    }

                    const std::size_t low = std::min(
                        lowWatermarkBytes,
                        highWatermarkBytes); // 低水位不得高于触发边界
                    m_lowWatermarkBytes.store(low, std::memory_order_release);
                    m_highWatermarkBytes.store(highWatermarkBytes, std::memory_order_release);
                }

                // 配置慢连接硬上限；0 表示不限制。
                void ConfigureLimit(std::size_t maxPendingBytes) noexcept {
                    m_maxPendingBytes.store(maxPendingBytes, std::memory_order_release);
                }

                // 写队列增长后返回至多一个互斥反馈信号。
                WriteQueueSignal ObserveGrowth(std::size_t pendingBytes) noexcept {
                    const std::size_t maxPending = m_maxPendingBytes.load(std::memory_order_acquire);
                    if (maxPending > 0 && pendingBytes > maxPending) return WriteQueueSignal::Overflow;

                    const std::size_t high = m_highWatermarkBytes.load(std::memory_order_acquire);
                    bool expected = false; // 同一高水位周期只允许首次增长赢得通知权
                    if (high > 0
                        && pendingBytes >= high
                        && m_highNotified.compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel)) {
                        return WriteQueueSignal::High;
                    }
                    return WriteQueueSignal::None;
                }

                // 写队列回落到低水位后解除 latch；并发禁用水位时不发送过期信号。
                WriteQueueSignal ObserveDrain(std::size_t pendingBytes) noexcept {
                    if (!m_highNotified.load(std::memory_order_acquire)) return WriteQueueSignal::None;

                    const std::size_t low = m_lowWatermarkBytes.load(std::memory_order_acquire);
                    bool expected = true; // CAS 与 ConfigureWatermark(0) 的 latch 重置竞争
                    if (pendingBytes <= low
                        && m_highNotified.compare_exchange_strong(
                            expected,
                            false,
                            std::memory_order_acq_rel)) {
                        return WriteQueueSignal::Low;
                    }
                    return WriteQueueSignal::None;
                }

            private:
                std::atomic<std::size_t> m_highWatermarkBytes{ 0 }; // 软背压触发字节数
                std::atomic<std::size_t> m_lowWatermarkBytes{ 0 };  // 软背压解除字节数
                std::atomic<std::size_t> m_maxPendingBytes{ 0 };    // 慢连接保护性关闭上限
                std::atomic<bool> m_highNotified{ false };         // 当前高水位周期通知 latch
            };
        }
    }
}
