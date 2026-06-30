#pragma once
#include <LikesProgram/Core/system/LikesProgramCoreExport.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <atomic>

namespace LikesProgram {
    namespace Time {
        // 高精度计时器
        class LIKESPROGRAM_CORE_API Timer {
        public:
            // 构造函数，默认不启动，父计时器为空
            explicit Timer(bool autoStart = false, Timer* parent = nullptr);
            // 使用父计时器构造，默认不自动启动。
            explicit Timer(Timer* parent) : Timer(false, parent) {}

            // 设置父计时器
            void SetParent(Timer* parent);
            ~Timer();

            // 拷贝计时器状态，running 状态会重新绑定当前时间点。
            Timer(const Timer& other);
            // 拷贝赋值计时器状态。
            Timer& operator=(const Timer& other);

            // 移动计时器内部状态和父计时器引用。
            Timer(Timer&& other) noexcept;
            // 移动赋值计时器状态和父计时器引用。
            Timer& operator=(Timer&& other) noexcept;

            // 开始计时
            void Start();

            // 停止计时
            Duration Stop();

            // 重置计时器
            void Reset();

            // 最近一次 Stop() 的耗时
            Duration GetLastElapsed() const;

            // 子计时器累计到父计时器的耗时
            Duration GetAccumulatedElapsed() const;

            // 当前是否在计时
            bool IsRunning() const;

            // 获取高精度纳秒时间
            static uint64_t NowNs();
        private:
            // PImpl 保存计时状态和累计值，避免头文件暴露原子字段。
            struct TimerImpl;
            TimerImpl* m_impl = nullptr; // 唯一拥有的计时器状态
            Timer* m_parent = nullptr;   // 可选父计时器，不拥有生命周期
        };
    }
}
