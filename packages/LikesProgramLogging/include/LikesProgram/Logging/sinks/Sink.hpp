#pragma once
#include <LikesProgram/Logging/system/LikesProgramLoggingExport.hpp>
#include <LikesProgram/Logging/LoggerType.hpp>
#include <string>

namespace LikesProgram {
    namespace Log {
        // 日志输出目标基类，所有具体 Sink 通过它接收格式化消息。
        class LIKESPROGRAM_LOGGING_API Sink {
        public:
            virtual ~Sink() = default;

            // 创建输出目标，sinkName 用于格式化日志时标识来源。
            explicit Sink(const String& sinkName = u"");

            // 写入一条日志记录，具体输出位置由子类决定。
            virtual void Write(const Message& message) = 0;

            // 将 Sink 内部缓冲尽量提交到底层输出，默认 Sink 无缓冲。
            virtual void Flush();

        protected:
            // 按统一格式生成日志文本，供控制台、文件和自定义 Sink 复用。
            const String FormatLogMessage(const Message& message);

            // 文件等字节 Sink 可直接取得目标编码结果；UTF-8 文本路径避免 String 往返转换。
            std::string FormatLogMessageBytes(const Message& message, String::Encoding encoding);

            // 复用调用方缓冲区生成字节结果，连续写入时避免逐条分配。
            void FormatLogMessageBytes(const Message& message, String::Encoding encoding,
                std::string& output);

        private:
            String m_sinkName; // 输出目标展示名，随 Sink 生命周期保存
        };
    }
}
