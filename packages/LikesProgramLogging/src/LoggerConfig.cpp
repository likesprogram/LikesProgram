#include <LikesProgram/Logging/LoggerConfig.hpp>

namespace LikesProgram {
    namespace Log {
        namespace {
            // 校验枚举值是否属于当前版本支持的队列溢出策略。
            bool IsKnownQueueOverflowPolicy(QueueOverflowPolicy policy) {
                switch (policy) {
                case QueueOverflowPolicy::Block:
                case QueueOverflowPolicy::DropNewest:
                case QueueOverflowPolicy::DropOldest:
                    return true;
                default:
                    return false;
                }
            }

            // 校验单个 Sink 配置，确保启用项有实例且重试策略合法。
            Status ValidateSinkConfig(const SinkConfig& config, size_t index) {
                if (!config.enabled) return Status::OkStatus();

                if (!config.sink) {
                    return Status::InvalidArgument(String::Format(
                        u"LoggerConfig.sinks[{}] is enabled but has no sink instance",
                        static_cast<uint64_t>(index)));
                }

                Status retryStatus = ValidateRetryConfig(config.retry); // 当前 Sink 的重试配置校验结果
                if (!retryStatus.IsOk()) {
                    return Status::InvalidArgument(String::Format(
                        u"LoggerConfig.sinks[{}].retry invalid: {}",
                        static_cast<uint64_t>(index), retryStatus.ToString()));
                }

                return Status::OkStatus();
            }
        }

        // 校验重试参数的边界，防止启用后出现无限队列或无效退避。
        Status ValidateRetryConfig(const RetryConfig& config) {
            if (!config.enabled) return Status::OkStatus();

            if (config.maxAttempts == 0) {
                return Status::InvalidArgument(u"enabled retry requires maxAttempts > 0");
            }

            if (config.maxQueueSize == 0) {
                return Status::InvalidArgument(u"enabled retry requires maxQueueSize > 0");
            }

            if (config.initialBackoff.count() < 0 || config.maxBackoff.count() < 0) {
                return Status::InvalidArgument(u"retry backoff durations must not be negative");
            }

            if (config.maxBackoff.count() > 0 && config.maxBackoff < config.initialBackoff) {
                return Status::InvalidArgument(u"retry maxBackoff must be zero or >= initialBackoff");
            }

            return Status::OkStatus();
        }

        // 校验完整 LoggerConfig，返回首个能直接定位字段的配置错误。
        Status ValidateLoggerConfig(const LoggerConfig& config) {
            if (!IsKnownQueueOverflowPolicy(config.options.overflowPolicy)) {
                return Status::InvalidArgument(u"LoggerConfig.options.overflowPolicy is invalid");
            }

            if (config.options.enqueueTimeout.count() < 0 &&
                config.options.overflowPolicy != QueueOverflowPolicy::Block) {
                return Status::InvalidArgument(
                    u"negative enqueueTimeout is only meaningful with Block overflow policy");
            }

            for (size_t i = 0; i < config.sinks.size(); ++i) {
                Status sinkStatus = ValidateSinkConfig(config.sinks[i], i); // 当前 Sink 的字段级校验结果
                if (!sinkStatus.IsOk()) return sinkStatus;
            }

            return Status::OkStatus();
        }

        // 将 ConsoleSink 便捷配置展开为通用 SinkConfig，并复用重试校验。
        Result<SinkConfig> MakeConsoleSinkConfig(const ConsoleSinkConfig& config) {
            Status retryStatus = ValidateRetryConfig(config.retry); // 控制台 Sink 的重试配置校验结果
            if (!retryStatus.IsOk()) return retryStatus;

            SinkConfig sinkConfig; // 输出给 Logger::ApplyConfig 的开放式 Sink 配置
            sinkConfig.name = config.name;
            sinkConfig.enabled = config.enabled;
            sinkConfig.minLevel = config.minLevel;
            sinkConfig.overrideOutputFormat = config.overrideOutputFormat;
            sinkConfig.outputFormat = config.outputFormat;
            sinkConfig.retry = config.retry;
            sinkConfig.sink = ConsoleSink::CreateSink();
            return sinkConfig;
        }

        // 将 FileSink 便捷配置展开为通用 SinkConfig，并创建真实 FileSink 实例。
        Result<SinkConfig> MakeFileSinkConfig(const FileSinkConfig& config) {
            Status retryStatus = ValidateRetryConfig(config.retry); // 文件 Sink 的重试配置校验结果
            if (!retryStatus.IsOk()) return retryStatus;

            if (config.multiProcess.lockTimeout.count() < 0) {
                return Status::InvalidArgument(u"multiProcess.lockTimeout must not be negative");
            }

            SinkConfig sinkConfig; // 输出给 Logger::ApplyConfig 的开放式 Sink 配置
            sinkConfig.name = config.name;
            sinkConfig.enabled = config.enabled;
            sinkConfig.minLevel = config.minLevel;
            sinkConfig.overrideOutputFormat = config.overrideOutputFormat;
            sinkConfig.outputFormat = config.outputFormat;
            sinkConfig.retry = config.retry;
            sinkConfig.sink = FileSink::CreateSink(config.path, config.filename,
                config.fileOptions, config.multiProcess);
            return sinkConfig;
        }
    }
}
