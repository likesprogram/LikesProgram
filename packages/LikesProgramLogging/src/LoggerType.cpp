#include <LikesProgram/Logging/LoggerType.hpp>

namespace LikesProgram {
    namespace Log {
        const String LevelToString(Level level) {
            // 展示文本保持旧版本拼写，便于日志文件和测试迁移。
            switch (level) {
            case Level::Trace: return u"Trace";
            case Level::Debug: return u"Debug";
            case Level::Info:  return u"Info";
            case Level::Warn:  return u"Warn";
            case Level::Error: return u"Error";
            case Level::Fatal: return u"Fatal";
            default: return u"Unknown";
            }
        }

        Level StringToLevel(const String& levelString, const Level defaultLevel) {
            String lowered = levelString.ToLower(); // 只转换一次，避免重复分配

            if (lowered == u"trace") return Level::Trace;
            if (lowered == u"debug") return Level::Debug;
            if (lowered == u"info") return Level::Info;
            if (lowered == u"warn") return Level::Warn;
            if (lowered == u"error") return Level::Error;
            if (lowered == u"fatal") return Level::Fatal;
            return defaultLevel;
        }
    }
}
