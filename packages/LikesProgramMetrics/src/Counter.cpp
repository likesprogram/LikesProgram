#include <LikesProgram/Metrics/Counter.hpp>
#include <metrics/MetricsInternal.hpp>

#include <atomic>
#include <cmath>

namespace LikesProgram {
    namespace Metrics {
        struct Counter::CounterImpl {
            std::atomic<double> m_value{ 0.0 }; // 当前累计计数值
        };

        Counter::Counter(const LikesProgram::String& name, const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : Metric(name, help, labels), m_impl(new CounterImpl{}) {
        }

        Counter::Counter(const Counter& other)
            : Metric(other), m_impl(new CounterImpl{}) {
            // 原子值按 relaxed 快照复制，Counter 不承诺跨对象强一致同步。
            if (other.m_impl) {
                m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
        }

        Counter& Counter::operator=(const Counter& other) {
            if (this == &other) return *this;

            Metric::operator=(other);
            if (!m_impl) m_impl = new CounterImpl{};
            const double value = other.m_impl
                ? other.m_impl->m_value.load(std::memory_order_relaxed)
                : 0.0; // moved-from 源对象按空计数处理
            m_impl->m_value.store(value, std::memory_order_relaxed);
            return *this;
        }

        Counter::Counter(Counter&& other) noexcept
            : Metric(std::move(other)), m_impl(other.m_impl) {
            // 直接接管实现对象，源对象只保留可析构空状态。
            other.m_impl = nullptr;
        }

        Counter& Counter::operator=(Counter&& other) noexcept {
            if (this == &other) return *this;

            Metric::operator=(std::move(other));
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Counter::~Counter() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Counter::Increment(double value) {
            // Counter 保持单调：负数、NaN 和 Inf 输入都不改变状态。
            if (!m_impl || value < 0.0 || !std::isfinite(value)) return;
            Internal::AddFiniteSaturating(m_impl->m_value, value);
        }

        double Counter::Value() const {
            // moved-from 对象按空 Counter 读取，保证诊断路径不崩溃。
            if (!m_impl) return 0.0;
            return Internal::FiniteForExport(m_impl->m_value.load(std::memory_order_relaxed));
        }

        void Counter::Reset() {
            if (!m_impl) m_impl = new CounterImpl{};
            m_impl->m_value.store(0.0, std::memory_order_relaxed);
        }

        LikesProgram::String Counter::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Counter::Labels() const {
            return LabelsCopy();
        }

        LikesProgram::String Counter::Help() const {
            return m_help;
        }

        LikesProgram::String Counter::Type() const {
            return u"counter";
        }

        LikesProgram::String Counter::ToPrometheus() const {
            const auto labels = LabelsCopy();              // 导出使用同一份标签快照
            const LikesProgram::String value = LikesProgram::String::Format(u"{:.6f}", Value());
            LikesProgram::String result = u"# HELP ";      // Prometheus 文本输出缓冲

            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ").Append(Type()).Append(u"\n");
            result.Append(m_name).Append(FormatLabels(labels)).Append(u" ").Append(value).Append(u"\n");
            return result;
        }

        LikesProgram::String Counter::ToJson() const {
            const auto labels = LabelsCopy();              // JSON 导出使用稳定标签快照
            LikesProgram::String json = u"{";

            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");
            json.Append(u"\"labels\":{");

            bool first = true;                             // JSON label 字段逗号控制
            for (const auto& [key, value] : labels) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
                first = false;
            }

            json.Append(u"},\"value\":")
                .Append(LikesProgram::String::Format(u"{:.6f}", Value()))
                .Append(u"}");
            return json;
        }
    }
}
