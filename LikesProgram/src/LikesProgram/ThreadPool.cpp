#include "../../include/LikesProgram/ThreadPool.hpp"
#include "../../include/LikesProgram/Timer.hpp"
#include "../../include/LikesProgram/CoreUtils.hpp"
#include "../../include/LikesProgram/math/Math.hpp"
#include <sstream>
#include <iomanip>

namespace LikesProgram {
    String ThreadPool::Statistics::ToString() const {
        LikesProgram::String statsStr;
        statsStr.Append(u"�ɹ���ӵ���������")
            .Append(LikesProgram::String(std::to_string(submitted))).Append(u"\r\n");
        statsStr.Append(u"���ܾ�����������")
            .Append(LikesProgram::String(std::to_string(rejected))).Append(u"\r\n");
        statsStr.Append(u"ִ����ɵ���������")
            .Append(LikesProgram::String(std::to_string(completed))).Append(u"\r\n");
        statsStr.Append(u"����ִ�е���������")
            .Append(LikesProgram::String(std::to_string(active))).Append(u"\r\n");
        statsStr.Append(u"�����߳�����")
            .Append(LikesProgram::String(std::to_string(aliveThreads))).Append(u"\r\n");
        statsStr.Append(u"��ʷ����߳�����")
            .Append(LikesProgram::String(std::to_string(largestPoolSize))).Append(u"\r\n");
        statsStr.Append(u"���з�ֵ��")
            .Append(LikesProgram::String(std::to_string(peakQueueSize))).Append(u"\r\n");

        if (lastSubmitTime.time_since_epoch().count() != 0) {
            auto tt = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lastSubmitTime - std::chrono::steady_clock::now()
                    + std::chrono::system_clock::now()));
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::wostringstream oss;
            oss << std::put_time(&tm, L"%F %T");
            statsStr.Append(u"���һ���ύʱ�䣺")
                .Append(LikesProgram::String(oss.str())).Append(u"\r\n");
        }

        if (lastFinishTime.time_since_epoch().count() != 0) {
            auto tt = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lastFinishTime - std::chrono::steady_clock::now()
                    + std::chrono::system_clock::now()));
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::wostringstream oss;
            oss << std::put_time(&tm, L"%F %T");
            statsStr.Append(u"���һ�����ʱ�䣺")
                .Append(LikesProgram::String(oss.str())).Append(u"\r\n");
        }

        statsStr.Append(u"������ʱ��")
            .Append(LikesProgram::Timer::ToString(longestTaskTime)).Append(u"\r\n");
        statsStr.Append(u"����ƽ�������ʱ��")
            .Append(LikesProgram::Timer::ToString(arithmeticAverageTaskTime)).Append(u"\r\n");
        statsStr.Append(u"ָ���ƶ�ƽ�������ʱ��")
            .Append(LikesProgram::Timer::ToString(averageTaskTime)).Append(u"\r\n");

        return statsStr;
    }

    ThreadPool::~ThreadPool() {
        try {
            Shutdown(ShutdownPolicy::CancelNow);
            NotifyAllWorkers();
            JoinAll(); // ����ֱ�������߳� exit & join
        }
        catch (...) {
            // �����в���
        }
    }

    void ThreadPool::Start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return; // �Ѿ�����
        }
        acceptTasks_.store(true, std::memory_order_release);

        // ������ coreThreads ��
        for (size_t i = 0; i < opts_.coreThreads; ++i) SpawnWorker();
    }

    void ThreadPool::Shutdown(ShutdownPolicy mode) {
        // �Ѿ�ֹͣ
        if (!running_.load(std::memory_order_acquire) &&
            !acceptTasks_.load(std::memory_order_acquire)) {
            return;
        }

        switch (mode) {
        case ShutdownPolicy::Graceful:
            // ���������������� worker ����ȡ����ֱ������������������������
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::Drain:
            // ���壺�ܾ������񣬵������������������ִ�С���Ҫ��ն��С�
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            break;
        case ShutdownPolicy::CancelNow:
            acceptTasks_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            shutdownNowFlag_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(queueMutex_);
                rejectedCount_.fetch_add(taskQueue_.size(), std::memory_order_relaxed);
                taskQueue_.clear();
            }
            break;
        }

        NotifyAllWorkers();
        queueNotFullCv_.notify_all(); // ��֤�ȴ���ӵ��߳�Ҳ�˳�
    }

    bool ThreadPool::AwaitTermination(std::chrono::milliseconds timeout) {
        if (timeout.count() == 0) {
            return aliveThreads_.load(std::memory_order_acquire) == 0;
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lk(workerExitMutex_);
        return workerExitCv_.wait_until(lk, deadline, [&] { return aliveThreads_.load(std::memory_order_acquire) == 0; });
    }

    bool ThreadPool::PostNoArg(std::function<void()> fn) {
        bool success = EnqueueTask(std::move(fn));
        if (!success && opts_.exceptionHandler) opts_.exceptionHandler(std::make_exception_ptr(std::runtime_error("Task rejected")));
        return success;
    }

    size_t ThreadPool::GetQueueSize() const {
        std::lock_guard<std::mutex> lk(queueMutex_);
        return taskQueue_.size();
    }

    ThreadPool::Statistics ThreadPool::Snapshot() const {
        Statistics s;
        s.submitted = submittedCount_.load();
        s.rejected = rejectedCount_.load();
        s.completed = completedCount_.load();
        s.active = activeCount_.load();
        s.aliveThreads = aliveThreads_.load();
        s.largestPoolSize = largestPoolSize_.load();
        s.peakQueueSize = peakQueueSize_.load();

        long long lastSubmitNs = lastSubmitNs_.load();
        long long lastFinishNs = lastFinishNs_.load();
        if (lastSubmitNs > 0)
            s.lastSubmitTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastSubmitNs));
        if (lastFinishNs > 0)
            s.lastFinishTime = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(lastFinishNs));

        s.longestTaskTime = timer_.GetLongestElapsed();
        s.averageTaskTime = timer_.GetEMAAverageElapsed();
        s.arithmeticAverageTaskTime = timer_.GetArithmeticAverageElapsed();
        return s;
    }

    void ThreadPool::JoinAll() {
        std::lock_guard<std::mutex> lk(workersMutex_);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    bool ThreadPool::EnqueueTask(std::function<void()>&& task) {
        if (!running_.load(std::memory_order_acquire) || !acceptTasks_.load(std::memory_order_acquire)) {
            rejectedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::unique_lock<std::mutex> lock(queueMutex_);

        if (taskQueue_.size() >= queueCapacity_) {
            switch (opts_.rejectPolicy) {
            case RejectPolicy::Block:
                queueNotFullCv_.wait(lock, [&] {
                    return taskQueue_.size() < queueCapacity_ || !acceptTasks_.load(std::memory_order_acquire) || shutdownNowFlag_.load(std::memory_order_acquire);
                    });
                if (!running_.load(std::memory_order_acquire) || !acceptTasks_.load(std::memory_order_acquire) || shutdownNowFlag_.load(std::memory_order_acquire)) {
                    rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                break;
            case RejectPolicy::Discard:
                rejectedCount_.fetch_add(1, std::memory_order_relaxed);
                return false;
            case RejectPolicy::DiscardOld:
                if (!taskQueue_.empty()) taskQueue_.pop_front();
                break;
            case RejectPolicy::Throw:
                throw std::runtime_error("ThreadPool: Task rejected (Throw policy)");
            }
        }

        taskQueue_.emplace_back(std::move(task));
        submittedCount_.fetch_add(1, std::memory_order_relaxed);

        // ��¼���һ���ύʱ�䣨ͳһʹ�����룩
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        lastSubmitNs_.store(now_ns, std::memory_order_relaxed);

        // ��ֵ����
        const size_t qsz = taskQueue_.size();
        Math::UpdateMax(peakQueueSize_, qsz);

        queueNotEmptyCv_.notify_one();

        // ��̬���ݣ����г��� > ��Ծ�߳��� �һ�û�� maxThreads
        if (opts_.allowDynamicResize && running_.load(std::memory_order_acquire)) {
            size_t alive = aliveThreads_.load(std::memory_order_acquire);
            if (alive < opts_.maxThreads && qsz > alive) {
                SpawnWorker();
            }
        }
        return true;
    }

    void ThreadPool::WorkerLoop() {
        // �߳���������ѡ��
        if (!opts_.threadNamePrefix.Empty()) {
            std::wostringstream woss;
            // ��ȡ��ǰ�̵߳� ID ��ֱ��ת��Ϊ������ͨ����һ��ָ��ֵ��
            std::thread::id threadId = std::this_thread::get_id();

            // �� std::thread::id ת��Ϊ����ֵ��ָ��ֵ����ֱ����ΪΨһ��ʶ��
            std::uintptr_t idValue = reinterpret_cast<std::uintptr_t>(&threadId);

            // ��֤����һ�� 5 λ�� ID��ͨ���� idValue ����ģ���㣩
            size_t threadIdNum = idValue % 100000;  // ��֤Ϊ 5 λ��

            opts_.threadNamePrefix = opts_.threadNamePrefix.SubString(0, 15 - 5);

            // ȷ�����Ϊ 5 λ��������ǰ�油�㣩
            woss << std::setw(5) << std::setfill(L'0') << threadIdNum;
            String threadName;
            threadName.Append(opts_.threadNamePrefix).Append(String(woss.str()));
            CoreUtils::SetCurrentThreadName(threadName);
        }

        // ������ʷ����߳�����spawnWorker �Ѱ� aliveThreads_++��
        {
            size_t cur = aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(largestPoolSize_, cur);
        }

        auto tryExit = [this]() -> bool {
            if (shutdownNowFlag_.load(std::memory_order_acquire)) return true;
            if (!running_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(queueMutex_);
                return taskQueue_.empty(); // drain ����м����˳�
            }
            return false;
            };

        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(queueMutex_);

                if (!shutdownNowFlag_.load(std::memory_order_acquire)) {
                    // ���еȴ����� keepAlive����������
                    queueNotEmptyCv_.wait_for(lock, opts_.keepAlive, [&] {
                        return !taskQueue_.empty() || shutdownNowFlag_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire);
                        });
                }

                // CancelNow ֱ����
                if (shutdownNowFlag_.load(std::memory_order_acquire)) break;

                // Graceful/Drain�����п��Ҳ������� -> �˳�
                if (!running_.load(std::memory_order_acquire) && taskQueue_.empty()) break;

                // ��̬���ݣ���ʱ�������߳������� core -> �˳�
                if (taskQueue_.empty() && opts_.allowDynamicResize && aliveThreads_.load(std::memory_order_acquire) > opts_.coreThreads) {
                    break;
                }

                if (!taskQueue_.empty()) {
                    task = std::move(taskQueue_.front());
                    taskQueue_.pop_front();
                    queueNotFullCv_.notify_one();
                }
            }

            if (task) {
                activeCount_.fetch_add(1, std::memory_order_relaxed);
                Timer timer(true, &timer_);
                try {
                    task();
                }
                catch (...) {
                    if (opts_.exceptionHandler) {
                        try { opts_.exceptionHandler(std::current_exception()); }
                        catch (...) {} // �ص��Լ��쳣Ҳ�̵�������ɱ��worker
                    }
                }
                auto elapsed = timer.Stop(); // �Զ�����ȫ�� EMA �����ʱ
                completedCount_.fetch_add(1, std::memory_order_relaxed);
                activeCount_.fetch_sub(1, std::memory_order_relaxed);
                lastFinishNs_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count(), std::memory_order_relaxed
                );
            }

            if (tryExit()) break;
        }

        // �߳��˳�
        {
            std::lock_guard<std::mutex> lk(workerExitMutex_);
            long long prev = aliveThreads_.load(std::memory_order_acquire);
            aliveThreads_.store((prev > 0 ? prev - 1 : 0), std::memory_order_release);
            if (aliveThreads_.load(std::memory_order_acquire) == 0) workerExitCv_.notify_all();
        }
    }

    void ThreadPool::SpawnWorker() {
        {
            std::lock_guard<std::mutex> lk(workerExitMutex_);
            aliveThreads_.fetch_add(1, std::memory_order_relaxed);
            size_t cur = aliveThreads_.load(std::memory_order_acquire);
            Math::UpdateMax(largestPoolSize_, cur);
        }

        std::thread t([this] { WorkerLoop(); });
        {
            std::lock_guard<std::mutex> lk(workersMutex_);
            workers_.emplace_back(std::move(t));
        }
    }

    void ThreadPool::NotifyAllWorkers() {
        queueNotEmptyCv_.notify_all();
        queueNotFullCv_.notify_all();
    }
}