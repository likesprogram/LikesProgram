#pragma once
#include "../../system/LikesProgramLibExport.hpp"
#include "../LoggerType.hpp"

namespace LikesProgram {
	namespace Log {
        // Sink 抽象接口
        class LIKESPROGRAM_API Sink {
        public:
            virtual ~Sink() = default;
            Sink(const String& sinkName = u"");

            // 写日志接口
            virtual void Write(const Message& message) = 0;
        protected:
            // 格式化日志内容
            const String FormatLogMessage(const Message& message);
        private:
            String m_sinkName;
        };
	}
}