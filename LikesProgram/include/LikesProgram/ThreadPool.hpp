#pragma once
#include "LikesProgramLibExport.hpp"
#include "String.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <tuple>
#include <utility>
#include <type_traits>
#include "Timer.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <stringapiset.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <pthread.h>
#endif

namespace LikesProgram {
    class LIKESPROGRAM_API ThreadPool { 
    public:
        // ����ܾ�����
        enum class RejectPolicy {
            Block,       // �����ȴ������п�λ
            Discard,     // ����������
            DiscardOld,  // �����������񣨶�ͷ����Ȼ�����������
            Throw        // �׳��쳣
        };
        // �رղ���
        enum class ShutdownPolicy {
            Graceful,   // ������������ִ������к����ܵ��������˳�
            Drain,      // ���ܾ̾�������ִ�ж������������񣻾����˳���������У�
            CancelNow   // ���ܾ̾������񣻶������У������˳�
        };
        // ����ѡ��
        struct Options {
            size_t coreThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1; // �����߳�
            size_t maxThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1;  // ����߳�
            size_t queueCapacity = 1024; // ��������
            RejectPolicy rejectPolicy = RejectPolicy::Block; // �ܾ�����
            std::chrono::milliseconds keepAlive = std::chrono::milliseconds(30000); // �����̻߳���ʱ��
            bool allowDynamicResize = true; // �Ƿ����ö�̬����/����
            String threadNamePrefix = u"tp-worker-"; // �߳���ǰ׺
            std::function<void(std::exception_ptr)> exceptionHandler = [](std::exception_ptr) {}; // �쳣�ص�
            // ���캯��
            Options(size_t coreThreads = 0, size_t maxThreads = 0, size_t queueCapacity = 1024,
                RejectPolicy rejectPolicy = RejectPolicy::Block,
                std::chrono::milliseconds keepAlive = std::chrono::milliseconds(30000),
                bool allowDynamicResize = true, String threadNamePrefix = u"tp-worker-",
                std::function<void(std::exception_ptr)> exceptionHandler = [](std::exception_ptr) {})
                : coreThreads(coreThreads ? coreThreads : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)),
                maxThreads(maxThreads ? maxThreads : (std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1)),
                queueCapacity(queueCapacity), rejectPolicy(rejectPolicy), keepAlive(keepAlive), allowDynamicResize(allowDynamicResize),
                threadNamePrefix(std::move(threadNamePrefix)), exceptionHandler(std::move(exceptionHandler)) {
            }
        };

        // ����ʱͳ����Ϣ
        struct Statistics {
            size_t submitted = 0;       // �ɹ���ӵ�������
            size_t rejected = 0;        // ���ܾ���������
            size_t completed = 0;       // ִ����ɵ�������
            size_t active = 0;          // ����ִ�е�������
            size_t aliveThreads = 0;    // �����߳���
            size_t largestPoolSize = 0; // ��ʷ����߳���
            size_t peakQueueSize = 0;   // ���з�ֵ
            std::chrono::steady_clock::time_point lastSubmitTime{};
            std::chrono::steady_clock::time_point lastFinishTime{};
            std::chrono::nanoseconds longestTaskTime{ 0 };
            std::chrono::nanoseconds arithmeticAverageTaskTime{ 0 };
            std::chrono::nanoseconds averageTaskTime{ 0 }; // EMA
        };

        explicit ThreadPool(Options opts = Options())
            : opts_(std::move(opts)),
            queueCapacity_(opts_.queueCapacity), timer_(Timer()) {
        }

        ~ThreadPool();

        // ---- �������ڿ��� ----
        // �����̳߳أ��ɶ�ε��õ�����һ����Ч��
        void Start();

        // ͳһ��ֹͣ��ڣ�����ԭ stop()/shutdownNow()
        void Shutdown(ShutdownPolicy mode = ShutdownPolicy::Graceful);

        // �����Ͻӿ�
        void Stop() { Shutdown(ShutdownPolicy::Graceful); }
        void ShutdownNow() { Shutdown(ShutdownPolicy::CancelNow); }

        // �ȴ�����worker�˳���timeout=0 ��ʾ���������
        bool AwaitTermination(std::chrono::milliseconds timeout);

        // ---- �ύ���� ----
        // �ύһ������,�в����ͷ���ֵ
        template<typename F, typename... Args>
        auto Submit(F&& f, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>> {
            using Ret = std::invoke_result_t<F, Args...>;

            // packaged_task that invokes fn with moved args (supports move-only args)
            auto task = std::make_shared<std::packaged_task<Ret()>>(
                [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Ret {
                    return std::apply(fn, std::move(tup));
                }
            );
            std::future<Ret> fut = task->get_future();

            auto wrapper = [task]() {
                // packaged_task ����쳣ת���� future
                (*task)();
                };

            if (!EnqueueTask(std::function<void()>(wrapper))) {
                // ���ʧ�ܣ�����һ���Ѿ����쳣�� future
                std::promise<Ret> p;
                p.set_exception(std::make_exception_ptr(std::runtime_error("ThreadPool: Task rejected")));
                return p.get_future();
            }
            return fut;
        }
        // �ύһ������,�޷���ֵ�޲���
        bool Post(std::function<void()> fn);

        // ---- ��ѯ���� ----
        size_t GetQueueSize() const;
        size_t GetActiveCount() const { return activeCount_.load(std::memory_order_acquire); } // ����ִ��������߳���
        size_t GetThreadCount() const { return aliveThreads_.load(std::memory_order_acquire); } // ���ء����š����߳���
        bool   IsRunning()     const { return running_.load(std::memory_order_acquire); }

        size_t IetRejectedCount() const { return rejectedCount_.load(std::memory_order_acquire); } // ���ܾ�����������
        size_t IetTotalTasksSubmitted() const { return submittedCount_.load(std::memory_order_acquire); }
        size_t IetCompletedCount() const { return completedCount_.load(std::memory_order_acquire); }
        size_t IetLargestPoolSize() const { return largestPoolSize_.load(std::memory_order_acquire); }
        size_t IetPeakQueueSize() const { return peakQueueSize_.load(std::memory_order_acquire); }

        // ��ȡ����ͳ����Ϣ
        Statistics Snapshot() const;

        // �ȴ������߳��˳���������
        void JoinAll();

    private:
        // ���ɿ���/�ƶ�
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        // ������Ӽ���̬����
        bool EnqueueTask(std::function<void()>&& task);

        // �����߳�ѭ����
        void WorkerLoop();

        // �������߳�
        void SpawnWorker();

        // �������й����߳�
        void NotifyAllWorkers();

        Options opts_;

        // �����Ķ���/ͬ���ṹ
        mutable std::mutex queueMutex_;
        std::condition_variable queueNotEmptyCv_; // ֪ͨ worker
        std::condition_variable queueNotFullCv_;  // ֪ͨ submit �ȴ�
        std::deque<std::function<void()>> taskQueue_; // �������
        size_t queueCapacity_; // ��������

        // �����߳����������ֵ�����join����Ծ���� aliveThreads_ ͳ�ƣ�
        std::vector<std::thread> workers_;
        mutable std::mutex workersMutex_;

        // ���б�־
        std::atomic<bool> running_{ false };       // ���Ƿ�������״̬������workerȡ����
        std::atomic<bool> acceptTasks_{ false };   // �Ƿ������������ӣ�
        std::atomic<bool> shutdownNowFlag_{ false }; // ���ڻ��ѿ��� worker �˳���shutdown��

        // ͳ��
        std::atomic<size_t> submittedCount_{ 0 };  // �ɹ���ӵ�������
        std::atomic<size_t> rejectedCount_{ 0 };   // ���ܾ���������
        std::atomic<size_t> completedCount_{ 0 };  // ���������
        std::atomic<size_t> activeCount_{ 0 };     // ����ִ�е�������
        std::atomic<size_t> aliveThreads_{ 0 };    // ����߳���
        std::atomic<size_t> largestPoolSize_{ 0 }; // ��ʷ����߳���
        std::atomic<size_t> peakQueueSize_{ 0 };   // ���з�ֵ
        Timer timer_;

        // ʱ��㣨���룩
        std::atomic<long long> lastSubmitNs_{ 0 };   // steady_clock::now().time_since_epoch() ��������
        std::atomic<long long> lastFinishNs_{ 0 };

        // �߳��˳��ȴ�
        mutable std::mutex workerExitMutex_;
        std::condition_variable workerExitCv_;
    };
}
