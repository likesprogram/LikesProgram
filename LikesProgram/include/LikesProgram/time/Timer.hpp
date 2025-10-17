#pragma once
#include "../LikesProgramLibExport.hpp"
#include "../String.hpp"
#include "../metrics/Summary.hpp"
#include "../metrics/Registry.hpp"
#include "Time.hpp"
#include <atomic>
#include <shared_mutex>

namespace LikesProgram {
	namespace Time {
        // 高精度计时器
        class LIKESPROGRAM_API Timer {
        public:
            // 构造函数, 默认不启动, 父计时器为空
            explicit Timer(std::shared_ptr<LikesProgram::Metrics::Summary> summary, bool autoStart = false, Timer* parent = nullptr);
            explicit Timer(bool autoStart) : Timer(nullptr, autoStart, nullptr) { }
            explicit Timer(Timer* parent) : Timer(nullptr, false, parent) {}
            explicit Timer(bool autoStart, Timer* parent) : Timer(nullptr, autoStart, parent) {}
            explicit Timer(std::shared_ptr<LikesProgram::Metrics::Summary> summary, Timer* parent) : Timer(summary, false, parent) {}
            Timer() : Timer(nullptr, false, nullptr) { }

            // 设置父计时器
            void SetParent(Timer* parent);
            ~Timer();

            // 拷贝构造 & 拷贝赋值
            Timer(const Timer& other);
            Timer& operator=(const Timer& other);

            // 移动构造 & 移动赋值
            Timer(Timer&& other) noexcept;
            Timer& operator=(Timer&& other) noexcept;

            // 开始计时
            void Start();

            // 停止计时
            Duration Stop();

            // 重置计时器
            void Reset();

            // 最近一次 Stop() 的耗时
            Duration GetLastElapsed() const;

            // 获取 Summary 统计对象
            const LikesProgram::Metrics::Summary& GetSummary() const;

            // 当前是否在计时
            bool IsRunning() const;

            // 获取高精度纳秒时间
            static uint64_t NowNs();
        private:
            struct TimerImpl;
            TimerImpl* m_impl = nullptr;
            Timer* m_parent = nullptr;
        };
	}
}
