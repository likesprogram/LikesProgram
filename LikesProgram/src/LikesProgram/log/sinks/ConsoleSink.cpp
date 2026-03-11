#include "../../../../include/LikesProgram/log/sinks/ConsoleSink.hpp"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

namespace LikesProgram {
	namespace Log {
		std::shared_ptr<Sink> ConsoleSink::CreateSink() {
			return std::make_shared<ConsoleSink>();
		}

        ConsoleSink::ConsoleSink() : Sink(u"ConsoleSink") { }

        void ConsoleSink::Write(const Message& message) {
            String formatted = FormatLogMessage(message);

#ifdef _WIN32
            HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO info;
            GetConsoleScreenBufferInfo(hConsole, &info);

            WORD color = info.wAttributes;
            switch (message.level) {
            case Level::Info:  color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED; break;
            case Level::Warn:  color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Level::Error: color = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            case Level::Fatal: color = BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
            case Level::Debug: color = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Level::Trace: color = FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
            }
            SetConsoleTextAttribute(hConsole, color);
            std::cout << formatted.ToStdString(message.encoding) << ' ';
            SetConsoleTextAttribute(hConsole, info.wAttributes);
            std::cout << "\b" << std::endl;
#else
            const char* colorCode = "";
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