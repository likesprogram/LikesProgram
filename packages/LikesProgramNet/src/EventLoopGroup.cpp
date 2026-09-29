#include <LikesProgram/Net/EventLoopGroup.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace LikesProgram {
    namespace Net {
        struct EventLoopGroup::EventLoopGroupImpl {
            explicit EventLoopGroupImpl(std::size_t workerCount)
                : m_workerCount(workerCount) {
            }

            std::size_t m_workerCount = 0;                     // 目标 worker 数量，构造后固定
            std::vector<std::shared_ptr<EventLoop>> m_loops;    // Group 与 worker lambda 共享 loop 生命周期
            std::vector<std::thread> m_threads;                 // worker 线程集合
            std::atomic<std::size_t> m_nextIndex{ 0 };          // 下一次分发使用的下标
            std::atomic<bool> m_running{ false };               // worker 组运行状态
            std::mutex m_mutex;                                 // 保护启动/停止过程
        };

        EventLoopGroup::EventLoopGroup(std::size_t workerCount)
            : m_impl(new EventLoopGroupImpl(workerCount)) {
        }

        EventLoopGroup::~EventLoopGroup() {
            Shutdown();
            delete m_impl;
            m_impl = nullptr;
        }

        void EventLoopGroup::Start() {
            if (m_impl == nullptr || m_impl->m_workerCount == 0) return;

            std::lock_guard<std::mutex> lock(m_impl->m_mutex); // 防止并发重复启动
            if (m_impl->m_running.load(std::memory_order_acquire)) return;

            m_impl->m_loops.clear();
            m_impl->m_threads.clear();
            m_impl->m_loops.reserve(m_impl->m_workerCount);
            m_impl->m_threads.reserve(m_impl->m_workerCount);

            std::mutex startedMutex;                  // 启动屏障互斥量
            std::condition_variable startedCv;         // 等待 worker 进入 loop
            std::size_t startedCount = 0;              // 已执行启动屏障任务的 worker 数

            try {
                for (std::size_t i = 0; i < m_impl->m_workerCount; ++i) {
                    auto loop = std::make_shared<EventLoop>(); // worker lambda 延长当前 loop 到 Start 返回
                    EventLoop* rawLoop = loop.get();
                    rawLoop->PostTask([&startedMutex, &startedCv, &startedCount]() {
                        std::lock_guard<std::mutex> startedLock(startedMutex);
                        ++startedCount;
                        startedCv.notify_one();
                    });

                    m_impl->m_loops.push_back(loop);
                    m_impl->m_threads.emplace_back([loop = std::move(loop)]() {
                        loop->Start();
                    });
                }

                {
                    std::unique_lock<std::mutex> startedLock(startedMutex);
                    startedCv.wait(startedLock, [this, &startedCount]() {
                        return startedCount == m_impl->m_workerCount;
                    });
                }

                for (const auto& loop : m_impl->m_loops) {
                    if (!loop || !loop->IsRunning()) {
                        throw std::runtime_error("EventLoopGroup Poller activation failed");
                    }
                }

                m_impl->m_nextIndex.store(0, std::memory_order_release);
                m_impl->m_running.store(true, std::memory_order_release);
            }
            catch (...) {
                for (auto& loop : m_impl->m_loops) {
                    if (loop) loop->Shutdown();
                }
                for (auto& thread : m_impl->m_threads) {
                    if (thread.joinable()) thread.join();
                }
                m_impl->m_threads.clear();
                m_impl->m_loops.clear();
                m_impl->m_nextIndex.store(0, std::memory_order_release);
                m_impl->m_running.store(false, std::memory_order_release);
                throw;
            }
        }

        void EventLoopGroup::Shutdown() {
            if (m_impl == nullptr) return;

            std::lock_guard<std::mutex> lock(m_impl->m_mutex); // 串行化停止和线程 join
            if (!m_impl->m_running.exchange(false, std::memory_order_acq_rel)
                && m_impl->m_threads.empty()) {
                return;
            }

            for (auto& loop : m_impl->m_loops) {
                if (loop) loop->Shutdown();
            }

            for (auto& thread : m_impl->m_threads) {
                if (!thread.joinable()) continue;

                // worker 内部触发关闭时不能 join 自己；lambda 会继续持有当前 loop 到 Start 返回。
                if (thread.get_id() == std::this_thread::get_id()) {
                    thread.detach();
                    continue;
                }

                // 由外部线程关闭时等待 worker loop 完整退出，避免连接生命周期悬空。
                thread.join();
            }

            m_impl->m_threads.clear();
            m_impl->m_loops.clear();
            m_impl->m_nextIndex.store(0, std::memory_order_release);
        }

        EventLoop* EventLoopGroup::NextLoop() noexcept {
            return NextLoopShared().get();
        }

        std::shared_ptr<EventLoop> EventLoopGroup::NextLoopShared() noexcept {
            if (m_impl == nullptr) return {};

            std::lock_guard<std::mutex> lock(m_impl->m_mutex); // 与 Shutdown 清空 worker 容器串行
            if (!m_impl->m_running.load(std::memory_order_acquire)
                || m_impl->m_loops.empty()) return {};

            const std::size_t index = m_impl->m_nextIndex.fetch_add(1, std::memory_order_acq_rel);
            return m_impl->m_loops[index % m_impl->m_loops.size()];
        }

        std::size_t EventLoopGroup::Size() const noexcept {
            return m_impl ? m_impl->m_workerCount : 0;
        }

        bool EventLoopGroup::IsRunning() const noexcept {
            return m_impl && m_impl->m_running.load(std::memory_order_acquire);
        }
    }
}
