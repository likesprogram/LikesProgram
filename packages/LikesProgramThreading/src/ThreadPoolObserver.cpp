#include <LikesProgram/Threading/ThreadPoolObserver.hpp>

namespace LikesProgram {
    namespace Threading {
        void ThreadPoolObserverBase::OnTaskSubmitted(const ThreadPoolEvent&) {
            // 默认空实现，允许用户只覆盖关心的事件。
        }

        void ThreadPoolObserverBase::OnTaskRejected(const ThreadPoolEvent&) {
            // 默认空实现，允许 Threading 在无 Metrics 情况下独立运行。
        }

        void ThreadPoolObserverBase::OnTaskStarted(const ThreadPoolEvent&) {
            // 默认空实现，避免强制用户实现所有事件。
        }

        void ThreadPoolObserverBase::OnTaskCompleted(const ThreadPoolEvent&) {
            // 默认空实现，兼容未来 Metrics 侧可选适配。
        }

        void ThreadPoolObserverBase::OnThreadCountAdded(const ThreadPoolEvent&) {
            // 默认空实现，线程生命周期事件可按需消费。
        }

        void ThreadPoolObserverBase::OnThreadCountRemoved(const ThreadPoolEvent&) {
            // 默认空实现，线程生命周期事件可按需消费。
        }
    }
}
