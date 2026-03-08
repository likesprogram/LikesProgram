#include "../../../include/LikesProgram/log/LoggerType.hpp"

namespace LikesProgram {
	namespace Log {
		const String LevelToString(Level level) {
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
            if (levelString.ToLower() == u"trace") return Level::Trace;
            if (levelString.ToLower() == u"debug") return Level::Debug;
            if (levelString.ToLower() == u"info") return Level::Info;
            if (levelString.ToLower() == u"warn") return Level::Warn;
            if (levelString.ToLower() == u"error") return Level::Error;
            if (levelString.ToLower() == u"fatal") return Level::Fatal;
            return defaultLevel;
        }
	}
}