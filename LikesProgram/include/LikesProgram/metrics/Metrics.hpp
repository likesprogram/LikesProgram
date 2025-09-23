#pragma once
#include "../LikesProgramLibExport.hpp"
#include "../String.hpp"
#include <unordered_map>

namespace LikesProgram {
	namespace Metrics {
		class LIKESPROGRAM_API MetricsObject {
		public:
			MetricsObject(const LikesProgram::String& name = u"", const LikesProgram::String& help = u"",
				const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels = {})
				: m_name(name), m_help(help), m_labels(labels) { }
			virtual ~MetricsObject() = default;

			virtual LikesProgram::String Name() const = 0;
			virtual std::unordered_map<LikesProgram::String, LikesProgram::String> Labels() const = 0;
            virtual LikesProgram::String Help() const = 0;
            virtual LikesProgram::String Type() const = 0;

			virtual LikesProgram::String ToPrometheus() const = 0;
			virtual LikesProgram::String ToJson() const = 0;

			static LikesProgram::String FormatLabels(const std::unordered_map<LikesProgram::String, LikesProgram::String>& labels);
		protected:
			LikesProgram::String m_name;
			LikesProgram::String m_help;
			std::unordered_map<LikesProgram::String, LikesProgram::String> m_labels;
		};
	}
}