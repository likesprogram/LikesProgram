#include "../../../include/LikesProgram/metrics/Summary.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"

namespace LikesProgram {
	namespace Metrics {
        Summary::Summary(const LikesProgram::String& name,
            size_t maxWindow, double alpha, const LikesProgram::String& help,
            const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels)
        : MetricsObject(name, help, labels), m_maxWindow(maxWindow), m_alpha(alpha) { }
        
        void Summary::Observe(double value) {
            UpdateStats(value);

            m_sketch.Add(value);
        }

        double Summary::Quantile(double q) const {
            if (q < 0.0 || q > 1.0) {
                throw std::invalid_argument("Quantile q must be between 0 and 1");
            }
            return m_sketch.Quantile(q);
        }

        void Summary::UpdateStats(double value) {
            m_count.fetch_add(1, std::memory_order_relaxed);
            m_sum.fetch_add(value, std::memory_order_relaxed);

            // EMA
            double prev = m_ema.load(std::memory_order_relaxed);
            double next = Math::EMA(prev, value, m_alpha);
            while (!m_ema.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
                next = Math::EMA(prev, value, m_alpha);
            }

            // Min / Max
            Math::UpdateMin(m_min, value);
            Math::UpdateMax(m_max, value);
        }

        double Summary::EMA() const {
            return m_ema.load(std::memory_order_relaxed);
        }

        double Summary::Average() const {
            return Math::Average(m_sum.load(std::memory_order_relaxed),
                m_count.load(std::memory_order_relaxed));
        }

        double Summary::Max() const {
            return m_max.load(std::memory_order_relaxed);
        }

        double Summary::Min() const {
            return m_min.load(std::memory_order_relaxed);
        }

        LikesProgram::String Summary::Name() const {
            return m_name;
        }

        std::unordered_map<LikesProgram::String, LikesProgram::String> Summary::Labels() const {
            return m_labels;
        }

        LikesProgram::String Summary::Help() const {
            return m_help;
        }

        LikesProgram::String Summary::Type() const {
            return u"summary";
        }

        LikesProgram::String Summary::ToPrometheus() const {
            LikesProgram::String result;
            auto appendMetric = [&](const LikesProgram::String& suffix, const LikesProgram::String& value) {
                result.Append(m_name);
                if (!suffix.Empty()) result.Append(suffix);
                if (!m_labels.empty()) {
                    result.Append(u"{");
                    bool first = true;
                    for (const auto& [k, v] : m_labels) {
                        if (!first) result.Append(u",");
                        result.Append(k).Append(u"=\"").Append(v).Append(u"\"");
                        first = false;
                    }
                    result.Append(u"}");
                }
                result.Append(u" ").Append(value).Append(u"\n");
            };

            auto appendQuantile = [&](double q, double val) {
                result.Append(m_name);
                if (!m_labels.empty() || true) { // 强制有 quantile 标签
                    result.Append(u"{");
                    bool first = true;
                    for (const auto& [k, v] : m_labels) {
                        if (!first) result.Append(u",");
                        result.Append(k).Append(u"=\"").Append(v).Append(u"\"");
                        first = false;
                    }
                    if (!first) result.Append(u",");
                    result.Append(u"quantile=\"").Append(LikesProgram::String::FromFloat(q, 2)).Append(u"\"");
                    result.Append(u"}");
                }
                result.Append(u" ").Append(LikesProgram::String::FromFloat(val, 6)).Append(u"\n");
            };

            appendMetric(u"_count", LikesProgram::String::FromInt(m_count.load()));
            appendMetric(u"_sum", LikesProgram::String::FromFloat(m_sum.load(), 6));

            appendQuantile(0.5, Quantile(0.5));
            appendQuantile(0.9, Quantile(0.9));
            appendQuantile(0.99, Quantile(0.99));

            appendMetric(u"_ema", LikesProgram::String::FromFloat(m_ema.load(), 6));
            appendMetric(u"_min", LikesProgram::String::FromFloat(m_min.load(), 6));
            appendMetric(u"_max", LikesProgram::String::FromFloat(m_max.load(), 6));

            return result;
        }

        LikesProgram::String Summary::ToJson() const {
            LikesProgram::String json;
            json.Append(u"{");
            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");

            json.Append(u"\"labels\":{");
            bool first = true;
            for (const auto& [k, v] : m_labels) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(k))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(v)).Append(u"\"");
                first = false;
            }
            json.Append(u"},");

            json.Append(u"\"count\":").Append(LikesProgram::String::FromInt(m_count.load())).Append(u",");
            json.Append(u"\"sum\":").Append(LikesProgram::String::FromFloat(m_sum.load(), 6)).Append(u",");

            json.Append(u"\"p50\":").Append(LikesProgram::String::FromFloat(Quantile(0.5), 6)).Append(u",");
            json.Append(u"\"p90\":").Append(LikesProgram::String::FromFloat(Quantile(0.9), 6)).Append(u",");
            json.Append(u"\"p99\":").Append(LikesProgram::String::FromFloat(Quantile(0.99), 6)).Append(u",");

            json.Append(u"\"ema\":").Append(LikesProgram::String::FromFloat(m_ema.load(), 6)).Append(u",");
            json.Append(u"\"min\":").Append(LikesProgram::String::FromFloat(m_min.load(), 6)).Append(u",");
            json.Append(u"\"max\":").Append(LikesProgram::String::FromFloat(m_max.load(), 6));
            json.Append(u"}");

            return json;
        }
	}
}