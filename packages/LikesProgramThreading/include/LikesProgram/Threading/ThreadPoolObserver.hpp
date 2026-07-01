#pragma once
#include <LikesProgram/Threading/system/LikesProgramThreadingExport.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <cstddef>

namespace LikesProgram {
    namespace Threading {
        // 线程池事件快照，供观察者或未来 Metrics 兼容层消费。
        struct ThreadPoolEvent {
            size_t queueSize = 0;              // 事件发生后的队列长度
            size_t activeTasks = 0;            // 事件发生时正在执行的任务数
            size_t aliveThreads = 0;           // 事件发生时存活工作线程数
            Time::Nanoseconds duration{ 0 };   // 任务完成事件的执行耗时
            Time::TimePoint timestamp{};       // 事件发生的系统时间
        };

        // 线程池观察者接口，不依赖 Metrics，适配层只需实现这些事件。
        class LIKESPROGRAM_THREADING_API IThreadPoolObserver {
        public:
            virtual ~IThreadPoolObserver() = default;

            // 任务成功提交后回调。
            virtual void OnTaskSubmitted(const ThreadPoolEvent& event) = 0;

            // 任务被拒绝或取消时回调。
            virtual void OnTaskRejected(const ThreadPoolEvent& event) = 0;

            // 任务开始执行时回调。
            virtual void OnTaskStarted(const ThreadPoolEvent& event) = 0;

            // 任务执行完成时回调。
            virtual void OnTaskCompleted(const ThreadPoolEvent& event) = 0;

            // 工作线程创建后回调。
            virtual void OnThreadCountAdded(const ThreadPoolEvent& event) = 0;

            // 工作线程退出后回调。
            virtual void OnThreadCountRemoved(const ThreadPoolEvent& event) = 0;
        };

        // 空观察者便于用户继承时只覆盖关心的事件。
        class LIKESPROGRAM_THREADING_API ThreadPoolObserverBase : public IThreadPoolObserver {
        public:
            // 默认不处理任务提交事件。
            void OnTaskSubmitted(const ThreadPoolEvent& event) override;

            // 默认不处理任务拒绝事件。
            void OnTaskRejected(const ThreadPoolEvent& event) override;

            // 默认不处理任务开始事件。
            void OnTaskStarted(const ThreadPoolEvent& event) override;

            // 默认不处理任务完成事件。
            void OnTaskCompleted(const ThreadPoolEvent& event) override;

            // 默认不处理线程新增事件。
            void OnThreadCountAdded(const ThreadPoolEvent& event) override;

            // 默认不处理线程减少事件。
            void OnThreadCountRemoved(const ThreadPoolEvent& event) override;
        };
    }
}
