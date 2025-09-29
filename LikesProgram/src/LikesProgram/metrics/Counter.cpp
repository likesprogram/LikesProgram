#include "../../../include/LikesProgram/metrics/Counter.hpp"

namespace LikesProgram {
	namespace Metrics {
		struct Counter::CounterImpl {
			std::atomic<double> m_value{ 0 };
		};

		Counter::Counter(const LikesProgram::String& name, const LikesProgram::String& help,
			const std::map<LikesProgram::String, LikesProgram::String>& labels)
		: MetricsObject(name, help, labels), m_impl(new CounterImpl{}) { }
		Counter::Counter(const Counter& other) : MetricsObject(other), m_impl(other.m_impl ? new CounterImpl{} : nullptr) {
			if (other.m_impl)
			m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
		}
		Counter& Counter::operator=(const Counter& other) {
			if (this != &other) {
				MetricsObject::operator=(other);
				if (other.m_impl) {
					if (!m_impl) m_impl = new CounterImpl{};
					m_impl->m_value.store(other.m_impl->m_value.load(std::memory_order_relaxed),
						std::memory_order_relaxed);
				}
				else {
					delete m_impl;
					m_impl = nullptr;
				}
			}
			return *this;
		}
		Counter::Counter(Counter&& other) noexcept : MetricsObject(std::move(other)), m_impl(other.m_impl) {
			other.m_impl = nullptr;
		}
		Counter& Counter::operator=(Counter&& other) noexcept {
			if (this != &other) {
				MetricsObject::operator=(std::move(other));
				delete m_impl;
				m_impl = other.m_impl;
				other.m_impl = nullptr;
			}
			return *this;
		}
		Counter::~Counter() {
			if (m_impl) delete m_impl;
			m_impl = nullptr;
		}

		void Counter::Increment(double value) {
			m_impl->m_value.fetch_add(value, std::memory_order_relaxed);
		}

		double Counter::Value() const {
			return m_impl->m_value.load(std::memory_order_relaxed);
		}

		void Counter::Reset() {
            m_impl->m_value.store(0.0, std::memory_order_relaxed);
		}

		LikesProgram::String Counter::Name() const {
			return m_name;
		}

		std::map<LikesProgram::String, LikesProgram::String> Counter::Labels() const {
			return GetLabels();
		}

		LikesProgram::String Counter::Help() const {
			return m_help;
		}

		LikesProgram::String Counter::Type() const {
			return u"counter";
		}

		LikesProgram::String Counter::ToPrometheus() const {
			LikesProgram::String result = u"# HELP ";
			result.Append(m_name).Append(u" ").Append(m_help).Append(u"\n");
			result.Append(u"# TYPE ").Append(m_name).Append(u" ");
            result.Append(Type()).Append(u"\n").Append(m_name);

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

			result.Append(u" ");
			result.Append(LikesProgram::String::FromFloat(m_impl->m_value.load(std::memory_order_relaxed), 6));
			result.Append(u"\n");
			return result;
		}

		LikesProgram::String Counter::ToJson() const {
			LikesProgram::String json = u"{";
			json.Append(u"\"name\":\"").Append(LikesProgram::String::EscapeJson(m_name)).Append(u"\",");
			json.Append(u"\"help\":\"").Append(LikesProgram::String::EscapeJson(m_help)).Append(u"\",");
			json.Append(u"\"type\":\"").Append(Type()).Append(u"\",");

			json.Append(u"\"labels\":{");
			bool first = true;
			for (const auto& [key, value] : GetLabels()) {
				if (!first) json.Append(u",");
				json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key));
				json.Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
				first = false;
			}
			json.Append(u"},");

			json.Append(u"\"value\":");
			json.Append(LikesProgram::String::FromFloat(m_impl->m_value.load(std::memory_order_relaxed), 6));
			json.Append(u"}");

			return json;
		}
	}
}