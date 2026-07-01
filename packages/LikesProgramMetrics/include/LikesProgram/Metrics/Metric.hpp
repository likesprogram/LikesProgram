#pragma once
#include <LikesProgram/Metrics/system/LikesProgramMetricsExport.hpp>
#include <LikesProgram/Core/String.hpp>

#include <map>

namespace LikesProgram {
    namespace Metrics {
        // 指标对象公共基类，统一保存名称、说明和标签导出契约。
        class LIKESPROGRAM_METRICS_API Metric {
        public:
            // 创建带基础元数据的指标对象。
            Metric(const LikesProgram::String& name = u"", const LikesProgram::String& help = u"",
                const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
            // 释放指标元数据实现对象。
            virtual ~Metric();

            // 返回指标名称，调用方应使用 Prometheus 兼容命名。
            virtual LikesProgram::String Name() const = 0;
            // 返回指标标签副本，避免跨线程暴露内部容器。
            virtual std::map<LikesProgram::String, LikesProgram::String> Labels() const = 0;
            // 返回指标帮助文本。
            virtual LikesProgram::String Help() const = 0;
            // 返回指标类型文本，例如 counter/gauge/histogram/summary。
            virtual LikesProgram::String Type() const = 0;

            // 清空当前指标采样值，保留名称、说明和标签。
            virtual void Reset() = 0;

            // 导出 Prometheus text exposition 片段。
            virtual LikesProgram::String ToPrometheus() const = 0;
            // 导出单个指标 JSON 对象。
            virtual LikesProgram::String ToJson() const = 0;

            // 深拷贝指标元数据。
            Metric(const Metric& other);
            // 深拷贝赋值指标元数据。
            Metric& operator=(const Metric& other);
            // 移动接管指标元数据。
            Metric(Metric&& other) noexcept;
            // 移动赋值指标元数据。
            Metric& operator=(Metric&& other) noexcept;

            // 将标签格式化为 Prometheus 标签块，空标签返回空字符串。
            static LikesProgram::String FormatLabels(
                const std::map<LikesProgram::String, LikesProgram::String>& labels);
        protected:
            LikesProgram::String m_name; // 指标名称，随对象生命周期保存
            LikesProgram::String m_help; // 指标帮助文本，导出时原样使用

            // 返回可写标签容器，延迟创建 moved-from 对象的实现。
            std::map<LikesProgram::String, LikesProgram::String>& MutableLabels();
            // 返回标签副本，调用方可以安全跨线程读取。
            std::map<LikesProgram::String, LikesProgram::String> LabelsCopy() const;
            // 替换指标标签，构造和赋值路径共用。
            void SetLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels);
        private:
            struct MetricImpl;
            MetricImpl* m_impl = nullptr; // 标签容器 PImpl，避免公共头暴露实现细节
        };

        // 旧版类型名兼容别名，便于从 LikesProgramOld 平滑迁移。
        using MetricsObject = Metric;
    }
}
