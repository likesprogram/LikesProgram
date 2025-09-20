#pragma once
#include "LikesProgramLibExport.hpp"
#include <chrono>
#include "String.hpp"
#include <atomic>
#include <shared_mutex>

namespace LikesProgram {
    // �߾��ȼ�ʱ��
    class LIKESPROGRAM_API Timer {
    public:
        using Duration = std::chrono::nanoseconds;
        explicit Timer(bool autoStart = false, Timer* parent = nullptr);
        void SetParent(Timer* parent);
        ~Timer() = default;
        
        // ��ʼ��ʱ
        void Start();

        // ֹͣ��ʱ
        Duration Stop(double alpha = 0.9);

        // ���ü�ʱ��
        void ResetThread();
        void ResetGlobal();
        void Reset();

        // ���һ�� Stop() �ĺ�ʱ
        Duration GetLastElapsed() const;

        // ȫ�� Stop() ���ۼƺ�ʱ
        Duration GetTotalElapsed() const;

        // ��ʷ���ʱ
        Duration GetLongestElapsed() const;

        // EMA ƽ����ʱ
        Duration GetEMAAverageElapsed() const;

        // ����ƽ����ʱ
        Duration GetArithmeticAverageElapsed() const;

        // ��ǰ�Ƿ��ڼ�ʱ
        bool IsRunning() const;

        // ��ʱ����ת��Ϊ�ַ���
        static String ToString(Duration duration);
    private:
        // ��ȡ�߾�������ʱ��
        static int64_t NowNs();
        Timer* m_parent = nullptr;

        std::atomic<int64_t> m_startNs = 0;
        std::atomic<int64_t> m_lastNs = 0;
        std::atomic<bool> m_running = false;
        std::atomic<long long> m_totalNs{ 0 };
        std::atomic<long long> m_count{ 0 };
        std::atomic<long long> m_longestNs{ 0 };
        std::atomic<double> m_averageNs{ 0 };

        mutable std::shared_mutex m_mutex;
    };
}
