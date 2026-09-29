#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            class ProvidedBufferPool final {
            public:
                using DrainFunction = void(*)(void*, std::uint32_t) noexcept;

                // 创建单 Poller 独占的 provided-buffer 存储与归还队列。
                static ProvidedBufferPool* Create(std::size_t bufferSize, std::uint32_t bufferCount);

                ProvidedBufferPool(const ProvidedBufferPool&) = delete;
                ProvidedBufferPool& operator=(const ProvidedBufferPool&) = delete;

                // 为即将交给 BufferLease 的 buffer 增加一份生命周期引用。
                void RetainLease(std::uint32_t token) noexcept;
                // 从任意线程归还 lease；只排队 token，不直接触碰 io_uring buffer ring。
                void ReleaseLease(std::uint32_t token) noexcept;
                // 在 EventLoop issuer 线程批量执行已排队的 buffer 归还。
                void DrainReturned(void* context, DrainFunction drainFunction) noexcept;
                // 注销 ring 后禁止回填，但保留 Poller owner 引用用于统计旧 lease 内存。
                void Retire() noexcept;
                // 关闭 Poller 对池的拥有关系；最后一个 lease 释放后销毁存储。
                void Close() noexcept;

                // 返回指定 provided-buffer 的稳定字节起点。
                std::uint8_t* BufferAddress(std::uint32_t token) noexcept;
                // 返回单个 provided-buffer 的固定容量。
                std::size_t BufferSize() const noexcept;
                // 返回当前池的 buffer 数量。
                std::uint32_t BufferCount() const noexcept;
                // 返回尚未归还到池的活动 lease 数量。
                std::size_t ActiveLeaseCount() const noexcept;
                // 返回已释放但尚未由 issuer 回填的 token 数量。
                std::size_t PendingReturnCount() const noexcept;
                // 返回当前可供内核选择的估算 buffer 数量。
                std::size_t CurrentAvailableCount() const noexcept;
                // 返回运行期观测到的最低可用 buffer 水位。
                std::size_t MinimumAvailableCount() const noexcept;
                // 返回 lease hold time 采样数量。
                std::uint64_t LeaseHoldSampleCount() const noexcept;
                // 返回采样 lease hold time 总纳秒数。
                std::uint64_t TotalLeaseHoldNanoseconds() const noexcept;
                // 返回采样 lease hold time 最大纳秒数。
                std::uint64_t MaximumLeaseHoldNanoseconds() const noexcept;

            private:
                // 一次性分配全部存储和两组无扩容归还队列。
                ProvidedBufferPool(std::size_t bufferSize, std::uint32_t bufferCount);
                // 仅由侵入式引用计数归零路径销毁。
                ~ProvidedBufferPool();

                // 释放 owner 或 lease 引用，最后一份引用负责 delete。
                void ReleaseReference() noexcept;

                std::atomic<std::uint32_t> m_referenceCount{ 1 }; // Poller owner 与活动 lease 的总引用数
                std::atomic<bool> m_closed{ false };              // 关闭后禁止任何 token 再回填 ring
                std::atomic<bool> m_ownerReleased{ false };       // Close 只释放一次 Poller owner 引用
                std::atomic<std::size_t> m_activeLeaseCount{ 0 }; // 当前仍由 BufferLease 持有的 buffer 数量
                std::atomic<std::size_t> m_pendingReturnCount{ 0 }; // 等待 issuer 回填 ring 的 token 数量
                std::atomic<std::size_t> m_unavailableBufferCount{ 0 }; // 未归还 ring 的权威 buffer 数量
                std::atomic<std::size_t> m_minimumAvailableCount{ 0 }; // 历史最低可用 buffer 水位
                std::atomic<std::uint64_t> m_leaseSequence{ 0 };  // 低开销 hold time 采样序号
                std::atomic<std::uint64_t> m_leaseHoldSamples{ 0 }; // 已完成的 hold time 样本数量
                std::atomic<std::uint64_t> m_totalLeaseHoldNs{ 0 }; // 样本 hold time 累计纳秒
                std::atomic<std::uint64_t> m_maximumLeaseHoldNs{ 0 }; // 样本最大 hold time 纳秒
                std::size_t m_bufferSize = 0;                     // 单个 buffer 的固定字节容量
                std::uint32_t m_bufferCount = 0;                  // 可由内核选择的 buffer 数量
                std::unique_ptr<std::atomic<std::uint64_t>[]> m_acquiredAtNs; // 每个 token 的采样起点，0 表示未采样
                std::vector<std::uint8_t> m_storage;              // lease 存活期间保持地址稳定的连续存储
                std::vector<std::uint32_t> m_returned;            // 跨线程等待 EventLoop drain 的 token
                std::vector<std::uint32_t> m_draining;            // EventLoop 当前批次使用的预分配 token 缓冲
                std::mutex m_returnMutex;                         // 只保护两组 token 队列的交换与追加
            };
        }
    }
}
