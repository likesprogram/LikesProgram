#pragma once
#include <LikesProgram/Metrics/Metric.hpp>

namespace LikesProgram {
    namespace Metrics {
        // 可增可减的瞬时值指标，适合温度、队列长度和连接数。
        class LIKESPROGRAM_METRICS_API Gauge : public Metric {
        public:
            // 创建 Gauge，初始值为 0。
            Gauge(const LikesProgram::String& name, const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 深拷贝 Gauge 当前值和元数据。
            Gauge(const Gauge& other);
            // 深拷贝赋值 Gauge 当前值和元数据。
            Gauge& operator=(const Gauge& other);
            // 移动接管 Gauge 实现对象。
            Gauge(Gauge&& other) noexcept;
            // 移动赋值 Gauge 实现对象。
            Gauge& operator=(Gauge&& other) noexcept;
            // 释放 Gauge 实现对象。
            ~Gauge() override;

            // 设置 Gauge 当前值。
            void Set(double value);
            // 增加 Gauge 当前值。
            void Increment(double value = 1.0);
            // 减少 Gauge 当前值。
            void Decrement(double value = 1.0);
            // 返回当前值。
            double Value() const;

            // 重置当前值为 0。
            void Reset() override;

            // 返回指标名称。
            LikesProgram::String Name() const override;
            // 返回标签副本。
            std::map<LikesProgram::String, LikesProgram::String> Labels() const override;
            // 返回帮助文本。
            LikesProgram::String Help() const override;
            // 返回 gauge 类型标识。
            LikesProgram::String Type() const override;
            // 导出 Prometheus gauge 文本。
            LikesProgram::String ToPrometheus() const override;
            // 导出 gauge JSON 对象。
            LikesProgram::String ToJson() const override;
        private:
            struct GaugeImpl;
            GaugeImpl* m_impl = nullptr; // 原子瞬时值实现
        };
    }
}
