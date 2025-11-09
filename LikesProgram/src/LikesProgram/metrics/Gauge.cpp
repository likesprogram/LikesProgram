#include "../../../include/LikesProgram/metrics/Gauge.hpp"
#include <atomic>

namespace LikesProgram {
	namespace Metrics {
        struct Gauge::GaugeImpl {
            std::atomic<double> m_value{ 0.0 };
        };

		Gauge::Gauge(const LikesProgram::String& name, const LikesProgram::String& help,
			const std::map<LikesProgram::String, LikesProgram::String>& labels)
        : MetricsObject(name, help, labels), m_impl(new GaugeImpl{}) { }
        Gauge::Gauge(const Gauge& other) : MetricsObject(other), m_impl(other.m_impl ? new GaugeImpl{} : nullptr) {
            if (other.m_impl) {
                m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }
        Gauge& Gauge::operator=(const Gauge& other) {
            if (this != &other) {
                MetricsObject::operator=(other);
                if (other.m_impl) {
                    if (!m_impl) m_impl = new GaugeImpl{};
                    m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                else {
                    delete m_impl;
                    m_impl = nullptr;
                }
            }
            return *this;
        }
        Gauge::Gauge(Gauge&& other) noexcept : MetricsObject(std::move(other)), m_impl(other.m_impl) {
            other.m_impl = nullptr;
        }
        Gauge& Gauge::operator=(Gauge&& other) noexcept {
            if (this != &other) {
                MetricsObject::operator=(std::move(other));
                delete m_impl;
                m_impl = other.m_impl;
                other.m_impl = nullptr;
            }
            return *this;
        }
        Gauge::~Gauge() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
        }

		void Gauge::Set(double value) {
            m_impl->m_value.store(value, std::memory_order_relaxed);
		}

		void Gauge::Increment(double value) {
            m_impl->m_value.fetch_add(value, std::memory_order_relaxed);
		}

		void Gauge::Decrement(double value) {
            m_impl->m_value.fetch_sub(value, std::memory_order_relaxed);
		}

        double Gauge::Value() const {
			return m_impl->m_value.load(std::memory_order_relaxed);
		}

        void Gauge::Reset() {
            m_impl->m_value.store(0.0, std::memory_order_relaxed);
        }

		LikesProgram::String Gauge::Name() const {
			return m_name;
		}

		std::map<LikesProgram::String, LikesProgram::String> Gauge::Labels() const {
			return GetLabels();
		}

		LikesProgram::String Gauge::Help() const {
			return m_help;
		}

		LikesProgram::String Gauge::Type() const {
			return u"gauge";
		}

        LikesProgram::String Gauge::ToPrometheus() const {
            LikesProgram::String result = u"# HELP ";
            result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
            result.Append(u"# TYPE ").Append(m_name).Append(u" ");
            result.Append(Type()).Append(u"\n");

            result.Append(m_name);
            if (!GetLabels().empty()) {
                result.Append(u"{");
                bool first = true;
                for (const auto& [k, v] : GetLabels()) {
                    if (!first) result.Append(u",");
                    result.Append(k).Append(u"=\"").Append(v).Append(u"\"");
                    first = false;
                }
                result.Append(u"}");
            }

            result.Append(u" ").Append(LikesProgram::String::Format(u"{:.6f}", m_impl->m_value.load(std::memory_order_relaxed)));
            result.Append(u"\n");

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
            for (const auto& [key, value] : GetLabels()) {
                if (!first) json.Append(u",");
                json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
                    .Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
                first = false;
            }
            json.Append(u"},");

            // 只保留原始值
            json.Append(u" ").Append(LikesProgram::String::Format(u"{:.6f}", m_impl->m_value.load(std::memory_order_relaxed)));
            json.Append(u"}");
            return json;
        }
	}
}
