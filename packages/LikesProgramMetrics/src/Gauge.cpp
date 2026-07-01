#include <LikesProgram/Metrics/Gauge.hpp>
#include <metrics/MetricsInternal.hpp>

#include <atomic>
#include <cmath>

namespace LikesProgram {
    namespace Metrics {
        struct Gauge::GaugeImpl {
            std::atomic<double> m_value{ 0.0 }; // 当前瞬时值
        };

        Gauge::Gauge(const LikesProgram::String& name, const LikesProgram::String& help,
            const std::map<LikesProgram::String, LikesProgram::String>& labels)
            : Metric(name, help, labels), m_impl(new GaugeImpl{}) {
        }

        Gauge::Gauge(const Gauge& other)
            : Metric(other), m_impl(new GaugeImpl{}) {
            // Gauge 快照复制不跨对象同步，只保留复制瞬间的原子值。
            if (other.m_impl) {
                m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
            }
        }

        Gauge& Gauge::operator=(const Gauge& other) {
            if (this == &other) return *this;

            Metric::operator=(other);
            if (!m_impl) m_impl = new GaugeImpl{};
            const double value = other.m_impl
                ? other.m_impl->m_value.load(std::memory_order_relaxed)
                : 0.0; // moved-from 源对象按默认 Gauge 处理
            m_impl->m_value.store(value, std::memory_order_relaxed);
            return *this;
        }

        Gauge::Gauge(Gauge&& other) noexcept
            : Metric(std::move(other)), m_impl(other.m_impl) {
            // 移动只转移实现指针，不复制原子值。
            other.m_impl = nullptr;
        }

        Gauge& Gauge::operator=(Gauge&& other) noexcept {
            if (this == &other) return *this;

            Metric::operator=(std::move(other));
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
            return *this;
        }

        Gauge::~Gauge() {
            delete m_impl;
            m_impl = nullptr;
        }

        void Gauge::Set(double value) {
            // NaN/Inf 不进入导出面，避免生成非法 JSON/Prometheus 数值。
            if (!std::isfinite(value)) return;
            if (!m_impl) m_impl = new GaugeImpl{};
            m_impl->m_value.store(value, std::memory_order_relaxed);
        }

        void Gauge::Increment(double value) {
            if (!m_impl || !std::isfinite(value)) return;
            Internal::AddFiniteSaturating(m_impl->m_value, value);
        }

        void Gauge::Decrement(double value) {
            if (!m_impl || !std::isfinite(value)) return;
            Internal::AddFiniteSaturating(m_impl->m_value, -value);
        }

        double Gauge::Value() const {
            if (!m_impl) return 0.0;
            return Internal::FiniteForExport(m_impl->m_value.load(std::memory_order_relaxed));
        }

        void Gauge::Reset() {
            if (!m_impl) m_impl = new GaugeImpl{};
            m_impl->m_value.store(0.0, std::memory_order_relaxed);
        }

        LikesProgram::String Gauge::Name() const {
            return m_name;
        }

        std::map<LikesProgram::String, LikesProgram::String> Gauge::Labels() const {
            return LabelsCopy();
        }

        LikesProgram::String Gauge::Help() const {
            return m_help;
        }

        LikesProgram::String Gauge::Type() const {
            return u"gauge";
        }

        LikesProgram::String Gauge::ToPrometheus() const {
            const auto labels = LabelsCopy();          // 标签快照保证本次导出自洽
            LikesProgram::String result = u"# HELP ";

            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ").Append(Type()).Append(u"\n");
            result.Append(m_name).Append(FormatLabels(labels)).Append(u" ")
                .Append(LikesProgram::String::Format(u"{:.6f}", Value())).Append(u"\n");
            return result;
        }

        LikesProgram::String Gauge::ToJson() const {
            const auto labels = LabelsCopy();          // JSON labels 输出前复制，避免重复读容器
            LikesProgram::String json = u"{";

            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");
            json.Append(u"\"labels\":{");

            bool first = true;                         // 控制 label 字段逗号
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
