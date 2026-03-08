#include "../../../../include/LikesProgram/log/sinks/Sink.hpp"
#include "../../../../include/LikesProgram/time/Time.hpp"
#include <iomanip>

namespace LikesProgram {
    namespace Log {
        Sink::Sink(const String& sinkName) {
            m_sinkName = sinkName.Empty() ? u"UnknownSink" : sinkName;
        }

        const String Sink::FormatLogMessage(const Message& message) {
            auto time_tc = std::chrono::system_clock::to_time_t(message.timestamp);
            auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(message.timestamp.time_since_epoch()) % 1000;

            std::wostringstream woss;
            // 时间
            std::tm tm = LikesProgram::Time::ToLocalTime(time_tc);
            woss << L"[" << std::put_time(&tm, L"%F %T") << L"." << std::setw(3) << std::setfill(L'0') << time_ms.count() << L"] ";

            // 线程信息
            woss << L"[T:";
            if (!message.threadName.Empty()) woss << message.threadName.ToWString();
            else woss << message.tid;
            woss << L"] ";

            // 输出器名称
            woss << L"[" << (m_sinkName.Empty() ? L"UnknownSink" : m_sinkName.ToWString()) << L"] ";

            // 日志级别
            woss << L"[" << LevelToString(message.level).ToWString() << L"] ";

            // 是否输出 调试信息
            if (message.debug) {
                // 函数信息
                woss << L"[Function:" << message.func << L"] ";

                // 文件信息
                woss << L"(" << message.file << L":" << message.line << L") ";
            }

            // 日志消息
            woss << message.msg.ToWString();

            return String(woss.str());
        }
    }
}