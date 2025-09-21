#define _CRT_SECURE_NO_WARNINGS
#include "../../include/LikesProgram/Logger.hpp"
#include "../../include/LikesProgram/CoreUtils.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <string>
#include <chrono>
#include <iomanip>
#include <thread>
#include <condition_variable>
#include <queue>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif
#include <shared_mutex>

namespace LikesProgram {
    // 实现 ILogSink
    String Logger::ILogSink::FormatLogMessage(const LogMessage& message, LogLevel minLevel) {
        auto t_c = std::chrono::system_clock::to_time_t(message.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            message.timestamp.time_since_epoch()) % 1000;

        std::wostringstream woss;
        // 时间
        woss << L"[" << std::put_time(std::localtime(&t_c), L"%F %T")
            << L"." << std::setw(3) << std::setfill(L'0') << ms.count() << L"] ";

        // 线程信息
        woss << L"[T:";
        if (!message.threadName.Empty()) woss << message.threadName.ToWString();
        else woss << message.tid;
        woss << L"] ";

        // 日志级别
        woss << L"[" << LevelToString(message.level).ToWString() << L"] ";
        // 是否输出文件信息
        if (minLevel <= LogLevel::Debug) woss << L"(" << message.file << L":" << message.line << L") ";
        // 日志消息
        woss << message.msg.ToWString();

        return String(woss.str());
    }

    const String Logger::ILogSink::LevelToString(LogLevel lvl) {
        switch (lvl) {
        case LogLevel::Trace: return u"TRACE";
        case LogLevel::Debug: return u"DEBUG";
        case LogLevel::Info:  return u"INFO";
        case LogLevel::Warn:  return u"WARN";
        case LogLevel::Error: return u"ERROR";
        case LogLevel::Fatal: return u"FATAL";
        }
        return u"UNKNOWN";
    }

    // ConsoleSink
    class ConsoleSink : public Logger::ILogSink {
    public:
        void Write(const Logger::LogMessage& message, Logger::LogLevel minLevel, String::Encoding encoding) override;
    };

    void ConsoleSink::Write(const Logger::LogMessage& message, Logger::LogLevel minLevel, String::Encoding encoding) {
        String formatted = FormatLogMessage(message, minLevel);

#ifdef _WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(hConsole, &info);

        WORD color = info.wAttributes;
        switch (message.level) {
        case Logger::LogLevel::Info:  color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED; break;
        case Logger::LogLevel::Warn:  color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Logger::LogLevel::Error: color = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
        case Logger::LogLevel::Fatal: color = BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        case Logger::LogLevel::Debug: color = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
        case Logger::LogLevel::Trace: color = FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
        }
        SetConsoleTextAttribute(hConsole, color);
        std::cout << formatted.ToStdString(encoding) << std::endl;
        SetConsoleTextAttribute(hConsole, info.wAttributes);
#else
        const char* colorCode = "";
        switch (message.level) {
        case LogLevel::Info:  colorCode = "\033[0m"; break;
        case LogLevel::Warn:  colorCode = "\033[33m"; break;
        case LogLevel::Error: colorCode = "\033[31m"; break;
        case LogLevel::Fatal: colorCode = "\033[41;97m"; break;
        case LogLevel::Debug: colorCode = "\033[32m"; break;
        case LogLevel::Trace: colorCode = "\033[34m"; break;
        }
        std::cout << colorCode << formatted.ToStdString(encoding) << "\033[0m" << std::endl;
#endif
    }


    // FileSink
    class FileSink : public Logger::ILogSink {
    public:
        explicit FileSink(const String& filename) {
            file.open(filename.ToStdString(), std::ios::out | std::ios::app);
        }
        void Write(const Logger::LogMessage& message, Logger::LogLevel minLevel, String::Encoding encoding) override {
            if (!file.is_open()) return;

            LikesProgram::String formatted = FormatLogMessage(message, minLevel);

            file << formatted.ToStdString(encoding) << std::endl;
        }
    private:
        std::ofstream file;
    };

    // Logger::Impl
    struct Logger::Impl {
        std::atomic<bool> stop{ false };
        LogLevel minLevel = LogLevel::Trace;

        std::shared_mutex sinkMtx;
        std::vector<std::shared_ptr<ILogSink>> sinks;

        std::mutex queueMtx;
        std::condition_variable cv;
        std::queue<LogMessage> queue;
        std::thread worker;
        String::Encoding encoding = String::Encoding::UTF8;

        Impl() {
            worker = std::thread([this]() { ProcessLoop(); });
        }

        ~Impl() {
            Shutdown();
        }

        void ProcessLoop() {
            while (!stop) {
                std::unique_lock<std::mutex> lock(queueMtx);
                cv.wait(lock, [this]() { return !queue.empty() || stop; });

                while (!queue.empty()) {
                    auto msg = queue.front();
                    queue.pop();
                    lock.unlock();

                    // 多个 sink 并发读
                    {
                        std::shared_lock<std::shared_mutex> sinkLock(sinkMtx);
                        for (auto& sink : sinks) {
                            sink->Write(msg, minLevel, encoding);
                        }
                    }

                    lock.lock();
                }
            }
        }

        void Shutdown() {
            stop = true;
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }
    };

    // Logger API
    Logger& Logger::Instance() {
        static Logger inst;
        if (inst.pImpl == nullptr || inst.pImpl->stop) {
            delete inst.pImpl;
            inst.pImpl = new Impl();
        }
        return inst;
    }

    Logger::Logger() {
        pImpl = new Impl();
    }

    Logger::~Logger() {
        delete pImpl;
    }

    void Logger::ProcessLoop()
    {
        pImpl->ProcessLoop();
    }

    void Logger::SetLevel(LogLevel level) {
        pImpl->minLevel = level;
    }

    void Logger::SetEncoding(String::Encoding encoding) {
        pImpl->encoding = encoding;
    }

    void Logger::AddSink(std::shared_ptr<ILogSink> sink) {
        std::unique_lock<std::shared_mutex> lock(pImpl->sinkMtx);
        pImpl->sinks.push_back(sink);
    }

    void Logger::Log(LogLevel level, const String& msg,
        const char* file, int line, const char* func)
    {
        if (level < pImpl->minLevel) return;

        LogMessage m;
        m.level = level;
        m.msg = msg;
        m.file = String(file);
        m.line = line;
        m.func = String(func);
        m.tid = std::this_thread::get_id();
        m.threadName = CoreUtils::GetCurrentThreadName();
        m.timestamp = std::chrono::system_clock::now();

        {
            std::lock_guard<std::mutex> lock(pImpl->queueMtx);
            pImpl->queue.push(m);
        }
        pImpl->cv.notify_one();
    }

    void Logger::Shutdown() {
        pImpl->Shutdown();
    }

    // ConsoleSink
    std::shared_ptr<Logger::ILogSink> CreateConsoleSink() {
        return std::make_shared<ConsoleSink>();
    }

    // FileSink
    std::shared_ptr<Logger::ILogSink> CreateFileSink(const String& filename) {
        return std::make_shared<FileSink>(filename);
    }
}
