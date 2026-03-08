#pragma once
#include "../String.hpp"
#include <thread>
#include <chrono>

namespace LikesProgram {
    namespace Log {
        enum class Level {
            Trace = 0,  // 最详细的日志, 用来跟踪程序的细粒度行为
            Debug,      // 详细日志, 用来记录开发过程中有用的信息
            Info,       // 运行时信息, 代表程序的正常运行状态
            Warn,       // 警告信息, 代表程序可能发生的错误
            Error,      // 错误信息, 程序出现了问题，某些功能可能失效
            Fatal       // 致命错误信息, 代表程序无法继续运行
        };

        // 日志级别转字符串
        const String LevelToString(Level level);
        // 字符串转日志级别
        Level StringToLevel(const String& levelString, const Level defaultLevel = Level::Trace);

        struct Message {
            Level level = Level::Trace;
            String msg;
            String file;
            int line = 0;
            std::thread::id tid;
            String threadName;
            std::chrono::system_clock::time_point timestamp;
            String func;
            Level minLevel = Level::Info;
            String::Encoding encoding = String::Encoding::UTF8;
            bool debug = true;
        };
    }
}