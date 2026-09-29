#include "net/platform/linux/ProvidedBufferPool.hpp"
#include <chrono>
#include <limits>
#include <stdexcept>

namespace {
    std::uint64_t SteadyNowNanoseconds() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void UpdateMaximum(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
        std::uint64_t current = target.load(std::memory_order_relaxed); // CAS 只在新最大值时竞争
        while (current < value
            && !target.compare_exchange_weak(
                current,
                value,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
        }
    }
}

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            ProvidedBufferPool* ProvidedBufferPool::Create(
                std::size_t bufferSize,
                std::uint32_t bufferCount) {
                return new ProvidedBufferPool(bufferSize, bufferCount);
            }

            ProvidedBufferPool::ProvidedBufferPool(
                std::size_t bufferSize,
                std::uint32_t bufferCount)
                : m_bufferSize(bufferSize),
                m_bufferCount(bufferCount) {
                if (m_bufferSize == 0 || m_bufferCount == 0) {
                    throw std::invalid_argument("Provided buffer pool requires non-zero dimensions");
                }
                if (m_bufferSize > std::numeric_limits<std::size_t>::max() / m_bufferCount) {
                    throw std::length_error("Provided buffer pool storage size overflow");
                }

                // 构造期完成全部可能分配，lease 归还热路径不得再扩容。
                m_storage.resize(m_bufferSize * m_bufferCount);
                m_returned.reserve(m_bufferCount);
                m_draining.reserve(m_bufferCount);
                m_acquiredAtNs = std::make_unique<std::atomic<std::uint64_t>[]>(m_bufferCount);
                for (std::uint32_t token = 0; token < m_bufferCount; ++token) {
                    m_acquiredAtNs[token].store(0, std::memory_order_relaxed);
                }
                m_minimumAvailableCount.store(m_bufferCount, std::memory_order_relaxed);
            }

            ProvidedBufferPool::~ProvidedBufferPool() = default;

            void ProvidedBufferPool::RetainLease(std::uint32_t token) noexcept {
                m_referenceCount.fetch_add(1, std::memory_order_relaxed);
                const std::size_t unavailable = m_unavailableBufferCount.fetch_add(
                    1,
                    std::memory_order_acq_rel) + 1; // 先减少可用量，避免并发快照短暂高估水位
                m_activeLeaseCount.fetch_add(1, std::memory_order_acq_rel);
                const std::size_t available = unavailable < m_bufferCount ? m_bufferCount - unavailable : 0;

                // 最低水位只向下更新，用于识别 provided-buffer 饥饿风险。
                std::size_t minimum = m_minimumAvailableCount.load(std::memory_order_relaxed);
                while (minimum > available
                    && !m_minimumAvailableCount.compare_exchange_weak(
                        minimum,
                        available,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                }

                // 只采样每 64 个 lease，限制 steady_clock 对接收热路径的扰动。
                if (token < m_bufferCount
                    && (m_leaseSequence.fetch_add(1, std::memory_order_relaxed) & 63U) == 0U) {
                    m_acquiredAtNs[token].store(SteadyNowNanoseconds(), std::memory_order_release);
                }
            }

            void ProvidedBufferPool::ReleaseLease(std::uint32_t token) noexcept {
                if (token < m_bufferCount) {
                    const std::uint64_t acquired = m_acquiredAtNs[token].exchange(0, std::memory_order_acq_rel);
                    if (acquired != 0) {
                        const std::uint64_t held = SteadyNowNanoseconds() - acquired; // 单调时钟采样时长
                        m_leaseHoldSamples.fetch_add(1, std::memory_order_relaxed);
                        m_totalLeaseHoldNs.fetch_add(held, std::memory_order_relaxed);
                        UpdateMaximum(m_maximumLeaseHoldNs, held);
                    }
                }
                m_activeLeaseCount.fetch_sub(1, std::memory_order_acq_rel);

                if (!m_closed.load(std::memory_order_acquire)) {
                    try {
                        std::lock_guard<std::mutex> lock(m_returnMutex); // 与 EventLoop 批量交换归还队列
                        if (!m_closed.load(std::memory_order_relaxed)
                            && token < m_bufferCount
                            && m_returned.size() < m_bufferCount) {
                            m_returned.push_back(token);
                            m_pendingReturnCount.fetch_add(1, std::memory_order_release);
                        }
                    }
                    catch (...) {
                        // 锁异常时宁可关闭期丢弃 token，也不能让 noexcept lease 析构终止进程。
                    }
                }

                ReleaseReference();
            }

            void ProvidedBufferPool::DrainReturned(
                void* context,
                DrainFunction drainFunction) noexcept {
                if (drainFunction == nullptr || m_closed.load(std::memory_order_acquire)) return;

                try {
                    std::lock_guard<std::mutex> lock(m_returnMutex); // 短临界区只交换两组预分配 vector
                    if (m_closed.load(std::memory_order_relaxed)) return;
                    m_returned.swap(m_draining);
                }
                catch (...) {
                    return;
                }

                // ring 回填只在调用 DrainReturned 的 EventLoop issuer 线程执行。
                for (const std::uint32_t token : m_draining) {
                    drainFunction(context, token);
                }
                m_pendingReturnCount.fetch_sub(m_draining.size(), std::memory_order_acq_rel);
                m_unavailableBufferCount.fetch_sub(m_draining.size(), std::memory_order_acq_rel);
                m_draining.clear();
            }

            void ProvidedBufferPool::Retire() noexcept {
                if (m_closed.exchange(true, std::memory_order_acq_rel)) return;

                try {
                    std::lock_guard<std::mutex> lock(m_returnMutex); // 关闭后排队 token 不再对应有效 ring
                    m_returned.clear();
                    m_draining.clear();
                    m_pendingReturnCount.store(0, std::memory_order_release);
                    m_unavailableBufferCount.store(m_bufferCount, std::memory_order_release);
                }
                catch (...) {
                    // closed 标志已发布，晚到 lease 只释放引用，不再访问 ring。
                }
            }

            void ProvidedBufferPool::Close() noexcept {
                Retire(); // 先阻止晚到 token 进入已注销 ring 的归还队列
                if (!m_ownerReleased.exchange(true, std::memory_order_acq_rel)) {
                    ReleaseReference();
                }
            }

            std::uint8_t* ProvidedBufferPool::BufferAddress(std::uint32_t token) noexcept {
                if (token >= m_bufferCount || m_storage.empty()) return nullptr;
                return m_storage.data() + static_cast<std::size_t>(token) * m_bufferSize;
            }

            std::size_t ProvidedBufferPool::BufferSize() const noexcept {
                return m_bufferSize;
            }

            std::uint32_t ProvidedBufferPool::BufferCount() const noexcept {
                return m_bufferCount;
            }

            std::size_t ProvidedBufferPool::ActiveLeaseCount() const noexcept {
                return m_activeLeaseCount.load(std::memory_order_acquire);
            }

            std::size_t ProvidedBufferPool::PendingReturnCount() const noexcept {
                return m_pendingReturnCount.load(std::memory_order_acquire);
            }

            std::size_t ProvidedBufferPool::CurrentAvailableCount() const noexcept {
                const std::size_t unavailable = m_unavailableBufferCount.load(std::memory_order_acquire); // 单原子权威水位
                return unavailable < m_bufferCount ? m_bufferCount - unavailable : 0;
            }

            std::size_t ProvidedBufferPool::MinimumAvailableCount() const noexcept {
                return m_minimumAvailableCount.load(std::memory_order_acquire);
            }

            std::uint64_t ProvidedBufferPool::LeaseHoldSampleCount() const noexcept {
                return m_leaseHoldSamples.load(std::memory_order_acquire);
            }

            std::uint64_t ProvidedBufferPool::TotalLeaseHoldNanoseconds() const noexcept {
                return m_totalLeaseHoldNs.load(std::memory_order_acquire);
            }

            std::uint64_t ProvidedBufferPool::MaximumLeaseHoldNanoseconds() const noexcept {
                return m_maximumLeaseHoldNs.load(std::memory_order_acquire);
            }

            void ProvidedBufferPool::ReleaseReference() noexcept {
                if (m_referenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
            }
        }
    }
}
