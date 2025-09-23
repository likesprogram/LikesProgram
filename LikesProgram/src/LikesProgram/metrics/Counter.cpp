#include "../../../include/LikesProgram/metrics/Counter.hpp"

namespace LikesProgram {
	namespace Metrics {
		Counter::Counter(const LikesProgram::String& name, const LikesProgram::String& help,
			const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels)
		: MetricsObject(name, help, labels) { }

		void Counter::Increment(int64_t value) {
			m_value.fetch_add(value, std::memory_order_relaxed);
		}

		int64_t Counter::Value() const {
			return m_value.load(std::memory_order_relaxed);
		}

		LikesProgram::String Counter::Name() const {
			return m_name;
		}

		std::unordered_map<LikesProgram::String, LikesProgram::String> Counter::Labels() const {
			return m_labels;
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

			if (!m_labels.empty()) {
				result.Append(u"{");
				bool first = true;
				for (const auto& [k, v] : m_labels) {
					if (!first) result.Append(u",");
					result.Append(k).Append(result).Append(u"=\"").Append(v).Append(u"\"");
					first = false;
				}
				result.Append(u"}");
			}

			result.Append(u" ");
			result.Append(LikesProgram::String::FromInt(m_value.load(std::memory_order_relaxed)));
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
			for (const auto& [key, value] : m_labels) {
				if (!first) json.Append(u",");
				json.Append(u"\"").Append(LikesProgram::String::EscapeJson(key));
				json.Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
				first = false;
			}
			json.Append(u"},");

			json.Append(u"\"value\":");
			json.Append(LikesProgram::String::FromInt(m_value.load(std::memory_order_relaxed)));
			json.Append(u"}");

			return json;
		}
	}
}