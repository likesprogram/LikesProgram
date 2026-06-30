#pragma once
#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <memory>

namespace LikesProgram {
    namespace Log {
        // 控制台输出 Sink，负责文本着色和标准输出写入。
        class LIKESPROGRAM_LOGGING_API ConsoleSink : public Sink {
        public:
            // 创建控制台 Sink，输出到当前进程标准输出。
            ConsoleSink();

            // 工厂函数用于 Logger::AddSink 的 shared_ptr 调用习惯。
            static std::shared_ptr<Sink> CreateSink();

            // 将日志写入控制台，并按日志级别应用颜色。
            void Write(const Message& message) override;
        };
    }
}
