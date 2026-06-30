#pragma once
#include <LikesProgram/Logging/sinks/Sink.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace LikesProgram {
    namespace Log {
        // 多进程写同一日志文件时的跨进程锁配置。
        struct MultiProcessFileConfig {
            bool enabled = false;                          // 是否启用跨进程文件锁
            String lockName;                               // 自定义锁名，空值时按日志路径自动派生
            std::chrono::milliseconds lockTimeout{ 100 };   // 等待跨进程锁的最长时间
        };

        // 文件 Sink 的轮转和保留策略。
        struct FileSinkOptions {
            size_t maxFileSizeMB = 30;           // 单文件最大 MB，0 表示不按大小轮转
            size_t maxFileSizeBytes = 0;         // 精确字节上限，非 0 时优先于 maxFileSizeMB
            uint32_t retentionDays = 0;          // 保留天数，0 表示不按文件修改时间清理
            size_t maxRetainedFiles = 0;         // 最多保留的托管日志文件数，0 表示不限制
            size_t maxTotalSizeBytes = 0;        // 托管日志文件总字节上限，0 表示不限制
        };

        // 文件输出 Sink，负责日期目录、轮转、保留和可选跨进程锁。
        class LIKESPROGRAM_LOGGING_API FileSink : public Sink {
        public:
            // 创建文件 Sink，path 为空时使用 ./logs，filename 为空时使用 Logger.log。
            explicit FileSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, size_t maxFileSizeMB = 30);

            // 使用完整文件策略创建 Sink，支持细粒度轮转和保留规则。
            explicit FileSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options);

            // 使用完整文件策略和跨进程文件锁配置创建 Sink。
            explicit FileSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options,
                const MultiProcessFileConfig& multiProcess);
            ~FileSink();

            FileSink(const FileSink&) = delete;
            FileSink& operator=(const FileSink&) = delete;
            FileSink(FileSink&&) noexcept = delete;
            FileSink& operator=(FileSink&&) noexcept = delete;

            // 将日志写入当前日期目录下的文件，必要时按大小或日期轮转。
            void Write(const Message& message) override;

            // 将文件流缓冲提交到底层文件句柄。
            void Flush() override;

            // 运行中更新轮转和保留策略，并立即尝试按新策略清理旧文件。
            void Configure(const FileSinkOptions& options);

            // 返回当前轮转和保留策略快照。
            FileSinkOptions Options() const;

            // 工厂函数用于 Logger::AddSink 的 shared_ptr 调用习惯。
            static std::shared_ptr<Sink> CreateSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, size_t maxFileSizeMB = 30);

            // 工厂函数用于传入完整文件策略。
            static std::shared_ptr<Sink> CreateSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options);

            // 工厂函数用于传入完整文件策略和跨进程文件锁配置。
            static std::shared_ptr<Sink> CreateSink(const LikesProgram::String& path,
                const LikesProgram::String& filename, const FileSinkOptions& options,
                const MultiProcessFileConfig& multiProcess);

        private:
            class FileSinkImpl;
            FileSinkImpl* m_impl = nullptr; // 唯一拥有的文件 Sink 实现对象
        };
    }
}
