#include <LikesProgram/Logging/sinks/ConsoleSink.hpp>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace LikesProgram {
    namespace Log {
        // 创建控制台 Sink 实例，返回基类指针以匹配 Logger::AddSink。
        std::shared_ptr<Sink> ConsoleSink::CreateSink() {
            // 工厂保持旧版 shared_ptr<Sink> 形态，方便 Logger::AddSink 直接使用。
            return std::make_shared<ConsoleSink>();
        }

        // 构造无状态控制台 Sink，名称用于格式化日志前缀。
        ConsoleSink::ConsoleSink() : Sink(u"ConsoleSink") {
            // 控制台 Sink 无额外状态，名称由基类保存。
        }

        // 将消息格式化后写入 stdout，并按日志级别设置平台控制台颜色。
        void ConsoleSink::Write(const Message& message) {
            String formatted = FormatLogMessage(message); // 统一前缀与正文格式

#ifdef _WIN32
            HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE); // 当前标准输出控制台
            CONSOLE_SCREEN_BUFFER_INFO info{}; // 保存原始颜色，写完后恢复
            if (!GetConsoleScreenBufferInfo(console, &info)) {
                std::cout << formatted.ToStdString(message.encoding) << std::endl;
                return;
            }

            WORD color = info.wAttributes; // 默认沿用当前控制台颜色
            switch (message.level) {
            case Level::Info:  color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED; break;
            case Level::Warn:  color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Level::Error: color = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            case Level::Fatal: color = BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
            case Level::Debug: color = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Level::Trace: color = FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
            }

            SetConsoleTextAttribute(console, color);
            std::cout << formatted.ToStdString(message.encoding) << std::endl;
            SetConsoleTextAttribute(console, info.wAttributes);
#else
            const char* colorCode = ""; // ANSI 颜色前缀，普通 Info 不额外加色
            switch (message.level) {
            case Level::Info:  colorCode = "\033[0m"; break;
            case Level::Warn:  colorCode = "\033[33m"; break;
            case Level::Error: colorCode = "\033[31m"; break;
            case Level::Fatal: colorCode = "\033[41;97m"; break;
            case Level::Debug: colorCode = "\033[32m"; break;
            case Level::Trace: colorCode = "\033[34m"; break;
            }

            std::cout << colorCode << formatted.ToStdString(message.encoding) << "\033[0m" << std::endl;
#endif
        }
    }
}
