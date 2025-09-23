#include "../../../include/LikesProgram/metrics/Gauge.hpp"
#include "../../../include/LikesProgram/math/Math.hpp"

namespace LikesProgram {
	namespace Metrics {
		Gauge::Gauge(const LikesProgram::String& name, const LikesProgram::String& help, double alpha,
			const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels)
		: MetricsObject(name, help, labels), m_alpha(alpha), m_ema(0.0),
            m_sum(0), m_count(0) {
            m_value.store(0, std::memory_order_relaxed);
        }

		void Gauge::Set(double value) {
			m_value.store(value, std::memory_order_relaxed);
		}

		void Gauge::Increment(double value) {
			m_value.fetch_add(value, std::memory_order_relaxed);
		}

		void Gauge::Decrement(double value) {
			m_value.fetch_sub(value, std::memory_order_relaxed);
		}

        double Gauge::Value() const {
			return m_value.load(std::memory_order_relaxed);
		}

		double Gauge::EMA() const {
			return m_ema.load(std::memory_order_relaxed);
		}

		double Gauge::Average() const {
			return Math::Average(m_sum.load(std::memory_order_relaxed),
				m_count.load(std::memory_order_relaxed));
		}

        double Gauge::Max() const {
			return m_max.load(std::memory_order_relaxed);
		}

        double Gauge::Min() const {
			return m_min.load(std::memory_order_relaxed);
		}

		LikesProgram::String Gauge::Name() const {
			return m_name;
		}

		std::unordered_map<LikesProgram::String, LikesProgram::String> Gauge::Labels() const {
			return m_labels;
		}

		LikesProgram::String Gauge::Help() const {
			return m_help;
		}

		LikesProgram::String Gauge::Type() const {
			return u"gauge";
		}

        LikesProgram::String Gauge::ToPrometheus() const {
            LikesProgram::String result;

            auto appendDerived = [&](const LikesProgram::String& metricName, const LikesProgram::String& type, const LikesProgram::String& value) {
                result.Append(u"# HELP ").Append(metricName).Append(u" Derived metric for ").Append(m_name).Append(u"\n");
                result.Append(u"# TYPE ").Append(metricName).Append(u" ").Append(type).Append(u"\n");

                result.Append(metricName);
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

            appendDerived(m_name, u"gauge", LikesProgram::String::FromFloat(m_value.load(std::memory_order_relaxed), 6));
            appendDerived(m_name + LikesProgram::String(u"_ema"), u"gauge", LikesProgram::String::FromFloat(m_ema.load(std::memory_order_relaxed), 6));
            appendDerived(m_name + u"_min", u"gauge", LikesProgram::String::FromFloat(m_min.load(std::memory_order_relaxed), 6));
            appendDerived(m_name + u"_max", u"gauge", LikesProgram::String::FromFloat(m_max.load(std::memory_order_relaxed), 6));
            appendDerived(m_name + u"_sum", u"gauge", LikesProgram::String::FromFloat(m_sum.load(std::memory_order_relaxed), 6));
            appendDerived(m_name + u"_count", u"gauge", LikesProgram::String::FromInt(m_count.load(std::memory_order_relaxed)));
            appendDerived(m_name + u"_avg", u"gauge", LikesProgram::String::FromFloat(Average(), 6));

            return result;
        }

        LikesProgram::String Gauge::ToJson() const {
            LikesProgram::String json;
            json.Append(u"{");
            json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
            json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
            json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");

            // labels
            json.Append(u"\"labels\":{");
            bool first = true;
            for (const auto& [key, value] : m_labels) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
                first = false;
            }
            json.Append(u"},");

            json.Append(u"\"value\":").Append(LikesProgram::String::FromFloat(m_value.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"ema\":").Append(LikesProgram::String::FromFloat(m_ema.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"min\":").Append(LikesProgram::String::FromFloat(m_min.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"max\":").Append(LikesProgram::String::FromFloat(m_max.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"sum\":").Append(LikesProgram::String::FromFloat(m_sum.load(std::memory_order_relaxed), 6)).Append(u",");
            json.Append(u"\"count\":").Append(LikesProgram::String::FromInt(m_count.load(std::memory_order_relaxed))).Append(u",");
            json.Append(u"\"average\":").Append(LikesProgram::String::FromFloat(Average(), 6));
            json.Append(u"}");

            return json;
        }

		void Gauge::UpdateStats(double value) {
			m_sum.fetch_add(value, std::memory_order_relaxed);
			m_count.fetch_add(1, std::memory_order_relaxed);

			// EMA
			double prev = m_ema.load(std::memory_order_relaxed);
			double next = Math::EMA(prev, value, m_alpha);
			while (!m_ema.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
				next = Math::EMA(prev, value, m_alpha);
			}

			Math::UpdateMax(m_max, value);
			Math::UpdateMin(m_min, value);
		}
	}
}
