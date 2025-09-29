#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
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
    struct Logger::LoggerImpl {
        std::atomic<bool> stop{ false };
        LogLevel minLevel = LogLevel::Trace;

        std::shared_mutex sinkMtx;
        std::vector<std::shared_ptr<ILogSink>> sinks;

        std::mutex queueMtx;
        std::condition_variable cv;
        std::queue<LogMessage> queue;
        std::thread worker;
        String::Encoding encoding = String::Encoding::UTF8;

        bool m_debug = false;
    };

    Logger& Logger::Instance() {
        return Instance(false, false);
    }

    Logger& Logger::Instance(bool autoStart) {
        return Instance(autoStart, false);
    }

    // Logger API
    Logger& Logger::Instance(bool autoStart, bool debug) {
        static Logger inst(autoStart, debug);
        if (inst.m_impl->stop.load(std::memory_order_acquire) && autoStart)
            inst.Start();
        return inst;
    }

    Logger::Logger(bool autoStart, bool debug) {
        m_impl = new LoggerImpl{};
        m_impl->m_debug = debug;
        m_impl->stop.store(true, std::memory_order_release); // 第一时间打 stop 标记
        if (autoStart) Start();
    }

    Logger::~Logger() {
        if (m_impl) {
            m_impl->stop.store(true, std::memory_order_release); // 第一时间打 stop 标记
            m_impl->cv.notify_all(); // 唤醒阻塞的 worker

            if (m_impl->worker.joinable()) m_impl->worker.join(); // 等待线程退出

            while (!m_impl->queue.empty()) m_impl->queue.pop();
            m_impl->sinks.clear();

            delete m_impl;
            m_impl = nullptr;
        }
    }

    void Logger::ProcessLoop() {
        while (!m_impl->stop) {
            std::unique_lock<std::mutex> lock(m_impl->queueMtx);
            m_impl->cv.wait(lock, [this]() { return !m_impl->queue.empty() || m_impl->stop; });

            if (m_impl->stop && m_impl->queue.empty()) break;

            while (!m_impl->queue.empty()) {
                auto msg = m_impl->queue.front();
                m_impl->queue.pop();
                lock.unlock();

                // 多个 sink 并发读
                {
                    std::shared_lock sinkLock(m_impl->sinkMtx); // 共享锁
                    for (auto& sink : m_impl->sinks) {
                        sink->Write(msg);
                    }
                }

                lock.lock();
            }
        }
    }

    void Logger::SetLevel(LogLevel level) {
        m_impl->minLevel = level;
    }

    void Logger::SetEncoding(String::Encoding encoding) {
        m_impl->encoding = encoding;
    }

    void Logger::AddSink(std::shared_ptr<ILogSink> sink) {
        std::unique_lock lock(m_impl->sinkMtx); // 独占锁
        m_impl->sinks.push_back(sink);
    }

    void Logger::Log(LogLevel level, const String& msg,
        const char* file, int line, const char* func)
    {
        if (!m_impl) return;
        if (m_impl->stop.load(std::memory_order_acquire)) return;
        if (level < m_impl->minLevel) return;

        try {
            LogMessage m;
            m.level = level;
            m.msg = String(msg);
            m.file = (file != nullptr) ? String(file) : String();
            m.line = line;
            m.tid = std::this_thread::get_id();
            m.threadName = CoreUtils::GetCurrentThreadName();
            m.timestamp = std::chrono::system_clock::now();
            m.func = (func != nullptr) ? String(func) : String();
            m.debug = m_impl->m_debug;
            m.minLevel = m_impl->minLevel;
            m.encoding = m_impl->encoding;
            // 使用 queueMtx 同步 stop 和 queue push
            {
                std::lock_guard<std::mutex> lock(m_impl->queueMtx);
                if (m_impl->stop.load(std::memory_order_acquire)) return;
                m_impl->queue.push(std::move(m));
            }
            m_impl->cv.notify_one();
        } catch (const std::exception& e) {
            std::cerr << "[Logger Error] Failed to log message: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[Logger Error] Failed to log message: Unknown exception" << std::endl;
        }
    }

    bool Logger::Start() {
        m_impl->stop.store(false, std::memory_order_release); // 设置停止标志
        if (!m_impl->worker.joinable())
            m_impl->worker = std::thread([this]() { ProcessLoop(); });
        return m_impl->worker.joinable();
    }

    void Logger::Shutdown(bool clearSink) {
        if (!m_impl) return;

        m_impl->stop.store(true, std::memory_order_release);

        m_impl->cv.notify_all(); // 唤醒 worker

        if (m_impl->worker.joinable()) {
            m_impl->worker.join();
        }

        if (clearSink) {
            std::unique_lock lock(m_impl->sinkMtx);
            m_impl->sinks.clear();
        }
    }

    // 实现 ILogSink
    String Logger::ILogSink::FormatLogMessage(const LogMessage& message) {
        auto t_c = std::chrono::system_clock::to_time_t(message.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            message.timestamp.time_since_epoch()) % 1000;

        std::wostringstream woss;
        // 时间
        woss << L"[" << std::put_time(std::localtime(&t_c), L"%F %T")
            << L"." << std::setw(3) << std::setfill(L'0') << ms.count() << L"] ";

        // 日志级别
        woss << L"[" << LevelToString(message.level).ToWString() << L"] ";

        // 是否输出 调试信息
        if (message.debug) {
            // 线程信息
            woss << L"[T:";
            if (!message.threadName.Empty()) woss << message.threadName.ToWString();
            else woss << message.tid;
            woss << L"] ";
            
            // 函数信息
            woss << L"[Function:" << message.func << L"] ";

            // 文件信息
            woss << L"(" << message.file << L":" << message.line << L") ";
        }

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
        void Write(const Logger::LogMessage& message) override {
            String formatted = FormatLogMessage(message);

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
            std::cout << formatted.ToStdString(message.encoding) << ' ';
            SetConsoleTextAttribute(hConsole, info.wAttributes);
            std::cout << "\b" << std::endl;
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
    };

    // FileSink
    class FileSink : public Logger::ILogSink {
    public:
        explicit FileSink(const String& filename) {
            file.open(filename.ToStdString(), std::ios::out | std::ios::app);
        }
        void Write(const Logger::LogMessage& message) override {
            if (!file.is_open()) return;

            LikesProgram::String formatted = FormatLogMessage(message);

            file << formatted.ToStdString(message.encoding) << std::endl;
        }
    private:
        std::ofstream file;
    };

    // ConsoleSink
    std::shared_ptr<Logger::ILogSink> Logger::CreateConsoleSink() {
        return std::make_shared<ConsoleSink>();
    }

    // FileSink
    std::shared_ptr<Logger::ILogSink> Logger::CreateFileSink(const String& filename) {
        return std::make_shared<FileSink>(filename);
    }
}
