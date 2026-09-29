#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace LikesProgram {
    namespace Log {
        namespace {
            // 将 std::thread::id 转为稳定文本，供文本日志和 JSON 日志复用。
            String ThreadIdToString(std::thread::id tid) {
                std::ostringstream idStream; // 将底层线程 id 转为可读文本
                idStream << tid;
                return String(idStream.str());
            }

            // 优先使用业务线程名，未设置时回退到底层线程 id。
            String DisplayThreadName(const Message& message) {
                if (!message.threadName.Empty()) return message.threadName;
                return ThreadIdToString(message.tid);
            }

            const char* LevelToUtf8(Level level) noexcept {
                switch (level) {
                case Level::Trace: return "Trace";
                case Level::Debug: return "Debug";
                case Level::Info: return "Info";
                case Level::Warn: return "Warn";
                case Level::Error: return "Error";
                case Level::Fatal: return "Fatal";
                default: return "Trace";
                }
            }

            void AppendUtf8(std::string& output, const String& value) {
                const char16_t* data = value.data(); // String 保证 UTF-16 内容合法且 NUL 终止
                const size_t size = value.Length();
                for (size_t index = 0; index < size; ++index) {
                    uint32_t codePoint = data[index]; // 当前 UTF-16 code point 或代理对高位
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < size) {
                        const uint32_t low = data[index + 1];
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                            ++index;
                        }
                    }

                    if (codePoint <= 0x7F) output.push_back(static_cast<char>(codePoint));
                    else if (codePoint <= 0x7FF) {
                        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                    }
                    else if (codePoint <= 0xFFFF) {
                        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                    }
                    else {
                        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                    }
                }
            }

            void AppendInteger(std::string& output, uint64_t value) {
                char buffer[32]{}; // uint64_t 十进制最长 20 字节
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
                output.append(buffer, result.ptr);
            }

            void AppendTimestamp(std::string& output,
                std::chrono::system_clock::time_point timestamp) {
                auto totalMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                    timestamp.time_since_epoch()).count();
                auto epochSeconds = totalMilliseconds / 1000;
                auto milliseconds = totalMilliseconds % 1000;
                if (milliseconds < 0) {
                    milliseconds += 1000;
                    --epochSeconds;
                }

                const auto secondPoint = std::chrono::system_clock::time_point(
                    std::chrono::seconds(epochSeconds));
                const std::time_t raw = std::chrono::system_clock::to_time_t(secondPoint);
                thread_local std::time_t cachedSecond = static_cast<std::time_t>(-1);
                thread_local std::string cachedPrefix;
                if (cachedPrefix.empty() || cachedSecond != raw) {
                    std::tm local{}; // 每秒只转换一次本地时间
#ifdef _WIN32
                    localtime_s(&local, &raw);
#else
                    localtime_r(&raw, &local);
#endif
                    char buffer[32]{}; // 缓存到秒的固定文本前缀，不依赖 locale
                    const int written = std::snprintf(buffer, sizeof(buffer),
                    "[%04d-%02d-%02d %02d:%02d:%02d.",
                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                    local.tm_hour, local.tm_min, local.tm_sec);
                    cachedPrefix.assign(buffer, written > 0 ? static_cast<size_t>(written) : 0);
                    cachedSecond = raw;
                }

                output.append(cachedPrefix);
                output.push_back(static_cast<char>('0' + (milliseconds / 100) % 10));
                output.push_back(static_cast<char>('0' + (milliseconds / 10) % 10));
                output.push_back(static_cast<char>('0' + milliseconds % 10));
                output.append("] ");
            }

            void AppendThreadId(std::string& output, std::thread::id threadId) {
                thread_local std::thread::id cachedId; // 同线程同步日志只格式化一次底层 id
                thread_local std::string cachedText;
                if (cachedText.empty() || cachedId != threadId) {
                    std::ostringstream stream;
                    stream << threadId;
                    cachedId = threadId;
                    cachedText = stream.str();
                }
                output.append(cachedText);
            }

            void AppendTextFieldUtf8(std::string& output, const char* name, const String& value) {
                if (value.Empty()) return;
                output.append(" [");
                output.append(name);
                output.push_back(':');
                AppendUtf8(output, value);
                output.push_back(']');
            }

            // 向文本日志追加非空上下文字段，字段为空时不输出冗余括号。
            void AppendTextField(String& text, const String& name, const String& value) {
                if (name.Empty() || value.Empty()) return;

                text.Append(String(u" ["));
                text.Append(name);
                text.Append(String(u":"));
                text.Append(value);
                text.Append(String(u"]"));
            }

            // 追加 Logger、模块、链路和自定义上下文字段。
            void AppendTextContext(String& text, const Message& message) {
                AppendTextField(text, u"Logger", message.loggerName);
                AppendTextField(text, u"Module", message.module);
                AppendTextField(text, u"Category", message.category);
                AppendTextField(text, u"TraceId", message.traceId);
                AppendTextField(text, u"SpanId", message.spanId);
                AppendTextField(text, u"RequestId", message.requestId);

                for (const auto& field : message.contextFields) {
                    if (field.key.Empty()) continue;
                    AppendTextField(text, String(u"Context.") + field.key, field.value);
                }
            }

            // 向 JSON 日志追加字符串属性，并维护逗号分隔状态。
            void AppendJsonStringProperty(String& text, const String& name,
                const String& value, bool& first) {
                if (value.Empty()) return;

                if (!first) text.Append(String(u","));
                first = false;

                text.Append(String(u"\""));
                text.Append(String::EscapeJson(name));
                text.Append(String(u"\":\""));
                text.Append(String::EscapeJson(value));
                text.Append(String(u"\""));
            }

            // 向 JSON 日志追加数值属性，并维护逗号分隔状态。
            void AppendJsonNumberProperty(String& text, const String& name,
                uint64_t value, bool& first) {
                if (!first) text.Append(String(u","));
                first = false;

                text.Append(String(u"\""));
                text.Append(String::EscapeJson(name));
                text.Append(String(u"\":"));
                text.Append(String(value));
            }

            // 构造 JSON Lines 日志对象，字段顺序固定以方便日志平台索引。
            String FormatJsonLogMessage(const Message& message, const String& sinkName) {
                String text(u"{"); // JSON Lines 单行对象缓冲
                bool first = true;  // 当前对象是否尚未写入属性

                AppendJsonStringProperty(text, u"timestamp",
                    LikesProgram::Time::FormatTime(message.timestamp, u"%Y-%m-%dT%H:%M:%S.%3f%z"), first);
                AppendJsonStringProperty(text, u"level", LevelToString(message.level), first);
                AppendJsonStringProperty(text, u"logger", message.loggerName, first);
                AppendJsonStringProperty(text, u"sink", sinkName, first);
                AppendJsonStringProperty(text, u"thread", DisplayThreadName(message), first);
                AppendJsonStringProperty(text, u"thread_id", ThreadIdToString(message.tid), first);
                AppendJsonNumberProperty(text, u"process_id", message.processId, first);
                AppendJsonStringProperty(text, u"module", message.module, first);
                AppendJsonStringProperty(text, u"category", message.category, first);
                AppendJsonStringProperty(text, u"trace_id", message.traceId, first);
                AppendJsonStringProperty(text, u"span_id", message.spanId, first);
                AppendJsonStringProperty(text, u"request_id", message.requestId, first);
                AppendJsonStringProperty(text, u"message", message.msg, first);

                if (message.debug) {
                    if (!first) text.Append(String(u","));
                    first = false;
                    text.Append(String(u"\"source\":{"));
                    bool sourceFirst = true; // source 子对象属性分隔状态
                    AppendJsonStringProperty(text, u"file", message.file, sourceFirst);
                    uint64_t sourceLine = message.line > 0 ? static_cast<uint64_t>(message.line) : 0; // source_location 行号快照
                    AppendJsonNumberProperty(text, u"line", sourceLine, sourceFirst);
                    AppendJsonStringProperty(text, u"function", message.func, sourceFirst);
                    text.Append(String(u"}"));
                }

                if (!message.contextFields.empty()) {
                    if (!first) text.Append(String(u","));
                    first = false;
                    text.Append(String(u"\"context\":{"));
                    bool contextFirst = true; // context 子对象属性分隔状态
                    for (const auto& field : message.contextFields) {
                        if (field.key.Empty()) continue;
                        AppendJsonStringProperty(text, field.key, field.value, contextFirst);
                    }
                    text.Append(String(u"}"));
                }

                text.Append(String(u"}"));
                return text;
            }
        }

        // 保存 Sink 展示名，空名称统一回退到 UnknownSink。
        Sink::Sink(const String& sinkName) {
            m_sinkName = sinkName.Empty() ? u"UnknownSink" : sinkName;
        }

        // 默认 Flush 无动作，带缓冲或文件句柄的 Sink 由派生类覆盖。
        void Sink::Flush() {
            // 默认 Sink 不持有缓冲，具体提交动作由子类覆盖。
        }

        // 按消息输出格式生成最终写入文本，文本格式和 JSON 格式共用上下文字段。
        const String Sink::FormatLogMessage(const Message& message) {
            if (message.outputFormat == LogOutputFormat::JsonLines) {
                return FormatJsonLogMessage(message, m_sinkName.Empty() ? String(u"UnknownSink") : m_sinkName);
            }

            String text = LikesProgram::Time::FormatTime(message.timestamp, u"[%Y-%m-%d %H:%M:%S.%3f] "); // 文本日志输出缓冲
            text.Append(String(u"[T:"));

            text.Append(DisplayThreadName(message));

            text.Append(String(u"] ["));
            text.Append(m_sinkName.Empty() ? String(u"UnknownSink") : m_sinkName);
            text.Append(String(u"] ["));
            text.Append(LevelToString(message.level));
            text.Append(String(u"]"));
            AppendTextContext(text, message);

            if (message.debug) {
                text.Append(String(u" "));
                text.Append(String(u"[Function:"));
                text.Append(message.func);
                text.Append(String(u"] ("));
                text.Append(message.file);
                text.Append(String(u":"));
                text.Append(String(static_cast<int64_t>(message.line)));
                text.Append(String(u") "));
            }
            else {
                text.Append(String(u" "));
            }

            text.Append(message.msg);
            return text;
        }

        std::string Sink::FormatLogMessageBytes(const Message& message, String::Encoding encoding) {
            std::string output;
            FormatLogMessageBytes(message, encoding, output);
            return output;
        }

        void Sink::FormatLogMessageBytes(const Message& message, String::Encoding encoding,
            std::string& output) {
            if (encoding != String::Encoding::UTF8 ||
                message.outputFormat != LogOutputFormat::Text) {
                output = FormatLogMessage(message).ToStdString(encoding);
                return;
            }

            output.clear(); // UTF-8 文本复用容量，避免 PImpl String 反复拼接和再编码
            output.reserve(192 + message.msg.Length());
            AppendTimestamp(output, message.timestamp);
            output.append("[T:");
            if (!message.threadName.Empty()) AppendUtf8(output, message.threadName);
            else AppendThreadId(output, message.tid);
            output.append("] [");
            AppendUtf8(output, m_sinkName);
            output.append("] [");
            output.append(LevelToUtf8(message.level));
            output.push_back(']');

            AppendTextFieldUtf8(output, "Logger", message.loggerName);
            AppendTextFieldUtf8(output, "Module", message.module);
            AppendTextFieldUtf8(output, "Category", message.category);
            AppendTextFieldUtf8(output, "TraceId", message.traceId);
            AppendTextFieldUtf8(output, "SpanId", message.spanId);
            AppendTextFieldUtf8(output, "RequestId", message.requestId);
            for (const auto& field : message.contextFields) {
                if (field.key.Empty() || field.value.Empty()) continue;
                output.append(" [Context.");
                AppendUtf8(output, field.key);
                output.push_back(':');
                AppendUtf8(output, field.value);
                output.push_back(']');
            }

            if (message.debug) {
                output.append(" [Function:");
                AppendUtf8(output, message.func);
                output.append("] (");
                AppendUtf8(output, message.file);
                output.push_back(':');
                AppendInteger(output, message.line > 0 ? static_cast<uint64_t>(message.line) : 0);
                output.append(") ");
            }
            else output.push_back(' ');

            AppendUtf8(output, message.msg);
        }
    }
}
