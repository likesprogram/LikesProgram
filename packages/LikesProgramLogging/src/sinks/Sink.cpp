#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <LikesProgram/Core/time/Time.hpp>
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
    }
}
