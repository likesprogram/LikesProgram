#pragma once

#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ProvidedBufferPolicy final {
            public:
                struct Observation {
                    std::size_t bufferSize = 0;             // 当前 generation 单 buffer 字节数
                    std::uint32_t bufferCount = 0;          // 当前 generation ring entry 数
                    std::size_t currentAvailable = 0;       // 当前窗口结束时的可用 buffer 水位
                    std::size_t activeLeases = 0;           // 当前仍由业务持有的 lease 数
                    std::uint64_t enobufsDelta = 0;         // 当前观察窗口新增 ENOBUFS 次数
                    std::uint64_t cqOverflowDelta = 0;      // CQ 消费拥塞信号，只阻止缩容，不驱动扩 ring
                    std::uint64_t receivedBytes = 0;        // 当前窗口成功接收的 payload 字节数
                    std::uint64_t receiveCompletions = 0;  // 当前窗口成功接收 completion 数
                    std::size_t existingGenerationBytes = 0; // 新 group 注册前仍存活的全部代内存
                };

                struct Recommendation {
                    std::size_t bufferSize = 0;             // 候选单 buffer 字节数
                    std::uint32_t bufferCount = 0;          // 候选 ring entry 数
                    bool changed = false;                   // false 表示保持当前 generation
                };

                // 校验新 generation 尺寸及其与现存 generation 的总内存峰值。
                static bool CanCreate(
                    std::size_t bufferSize,
                    std::uint32_t bufferCount,
                    std::size_t existingGenerationBytes) noexcept;
                // 根据窗口反馈、内存上界与迟滞生成下一代配置。
                Recommendation Observe(const Observation& observation) noexcept;

            private:
                std::uint32_t m_pressureSamples = 0;        // 连续高压力窗口数
                std::uint32_t m_calmSamples = 0;            // 连续低占用窗口数
                std::uint32_t m_cooldownSamples = 0;        // 切换后禁止再次调整的观察窗口数
            };
        }
    }
}
