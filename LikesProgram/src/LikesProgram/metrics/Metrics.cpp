#include "../../../include/LikesProgram/metrics/Metrics.hpp"

namespace LikesProgram {
	namespace Metrics {
		LikesProgram::String MetricsObject::FormatLabels(const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels) {
			LikesProgram::String result = u"{";
            bool first = true;
			for (auto& [key, value] : labels) {
				if (!key.Empty()) {
                	if (!first) result.Append(u",");
					if (!value.Empty()) {
						result.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
						.Append(u"\":\"").Append(LikesProgram::String::EscapeJson(value)).Append(u"\"");
					}
					else {
						result.Append(u"\"").Append(LikesProgram::String::EscapeJson(key))
						.Append(u"\":\"").Append(LikesProgram::String::FromBool(false)).Append(u"\"");
					}
					first = false;
				}
			}
			return result.Append(u"}");
		}
	}
}
