#include "../../../include/LikesProgram/metrics/Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
		struct MetricsObject::MetricsObjectImpl {
			std::map<LikesProgram::String, LikesProgram::String> m_labels;
		};

		MetricsObject::MetricsObject(const LikesProgram::String& name, const LikesProgram::String& help, const std::map<LikesProgram::String, LikesProgram::String>& labels)
		: m_name(name), m_help(help), m_impl(nullptr) { SetLabels(labels); }
		MetricsObject::~MetricsObject() {
            if (m_impl) delete m_impl;
            m_impl = nullptr;
		}
		MetricsObject::MetricsObject(const MetricsObject& other)
			: m_name(other.m_name), m_help(other.m_help), m_impl(nullptr) {
			if (other.m_impl) {
				m_impl = new MetricsObjectImpl(*other.m_impl);
			}
		}
		MetricsObject& MetricsObject::operator=(const MetricsObject& other) {
			if (this != &other) {
				m_name = other.m_name;
				m_help = other.m_help;
				if (other.m_impl) {
					if (!m_impl) m_impl = new MetricsObjectImpl(*other.m_impl);
					else *m_impl = *other.m_impl;
				}
				else {
					delete m_impl;
					m_impl = nullptr;
				}
			}
			return *this;
		}
		MetricsObject::MetricsObject(MetricsObject&& other) noexcept
		: m_name(std::move(other.m_name)), m_help(std::move(other.m_help)), m_impl(other.m_impl) {
			other.m_impl = nullptr;
		}
		MetricsObject& MetricsObject::operator=(MetricsObject&& other) noexcept {
			if (this != &other) {
				delete m_impl;
				m_name = std::move(other.m_name);
				m_help = std::move(other.m_help);
				m_impl = other.m_impl;
				other.m_impl = nullptr;
			}
			return *this;
		}

		LikesProgram::String MetricsObject::FormatLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels) {
			LikesProgram::String result = u"{";
            bool first = true;
			for (auto& [key, value] : labels) {
				if (!key.Empty()) {
                	if (!first) result.Append(u",");
					result.Append(key).Append(u"=\"");
					if (!value.Empty()) result.Append(value);
					result.Append(u"\"");
					first = false;
				}
			}
			return result.Append(u"}");
		}

		std::map<LikesProgram::String, LikesProgram::String>& MetricsObject::GetLabels() {
			if (!m_impl) m_impl = new MetricsObjectImpl{};
			return m_impl->m_labels;
		}

		std::map<LikesProgram::String, LikesProgram::String> MetricsObject::GetLabels() const {
			if (!m_impl) return std::map<LikesProgram::String, LikesProgram::String>();
			return m_impl->m_labels;
		}

		void MetricsObject::SetLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels) {
			if (!m_impl) m_impl = new MetricsObjectImpl{};
			m_impl->m_labels = labels;
		}
	}
}
