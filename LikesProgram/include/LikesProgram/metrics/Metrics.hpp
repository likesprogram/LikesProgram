#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include "../String.hpp"
#include <map>

namespace LikesProgram {
	namespace Metrics {
		class LIKESPROGRAM_API MetricsObject {
		public:
			MetricsObject(const LikesProgram::String& name = u"", const LikesProgram::String& help = u"",
				const std::map<LikesProgram::String, LikesProgram::String>& labels = {});
			virtual ~MetricsObject();

			virtual LikesProgram::String Name() const = 0;
			virtual std::map<LikesProgram::String, LikesProgram::String> Labels() const = 0;
            virtual LikesProgram::String Help() const = 0;
            virtual LikesProgram::String Type() const = 0;

			virtual void Reset() = 0;

			virtual LikesProgram::String ToPrometheus() const = 0;
			virtual LikesProgram::String ToJson() const = 0;

			MetricsObject(const MetricsObject& other);
			MetricsObject& operator=(const MetricsObject& other);
			MetricsObject(MetricsObject&& other) noexcept;
			MetricsObject& operator=(MetricsObject&& other) noexcept;

			static LikesProgram::String FormatLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels);
		protected:
			LikesProgram::String m_name;
			LikesProgram::String m_help;
			std::map<LikesProgram::String, LikesProgram::String>& GetLabels();
			std::map<LikesProgram::String, LikesProgram::String> GetLabels() const;
			void SetLabels(const std::map<LikesProgram::String, LikesProgram::String>& labels);
		private:
			struct MetricsObjectImpl;
			MetricsObjectImpl* m_impl;
		};
	}
}