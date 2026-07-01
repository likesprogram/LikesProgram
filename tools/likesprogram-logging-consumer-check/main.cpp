#include <LikesProgram/Logging/Logging.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>

namespace {
    class CountingSink final : public LikesProgram::Log::Sink {
    public:
        // 创建外部消费方自定义 Sink，验证继承式扩展入口可用。
        CountingSink()
            : Sink(u"consumer-counting-sink") {
        }

        // 记录收到的日志消息数量。
        void Write(const LikesProgram::Log::Message&) override {
            std::lock_guard<std::mutex> lock(m_mutex); // 保护跨线程写入计数
            ++m_count;
        }

        // 返回当前累计消息数。
        std::size_t Count() const {
            std::lock_guard<std::mutex> lock(m_mutex); // 与后台日志线程同步
            return m_count;
        }

    private:
        mutable std::mutex m_mutex; // 保护 m_count 的互斥锁
        std::size_t m_count = 0;    // 已接收日志消息数
    };
}

int main() {
    if (!LikesProgram::Logging::PackageAvailable()) return 1;

    auto& logger = LikesProgram::Log::Logger::Instance(false, false); // 全局 Logger 诊断对象
    logger.Shutdown(true);
    logger.SetLevel(LikesProgram::Log::Level::Trace);

    LikesProgram::Log::LoggerOptions options; // 外部消费方最小队列配置
    options.maxQueueSize = 32;
    logger.Configure(options);

    auto sink = std::make_shared<CountingSink>(); // 验证自定义 Sink 注入和分发
    logger.AddSink(sink);
    if (!logger.Start()) return 2;

    logger.Log(LikesProgram::Log::Level::Info, std::source_location::current(),
        u"logging consumer check");
    if (!logger.Flush(std::chrono::seconds(5))) return 3;
    if (!logger.Shutdown(std::chrono::seconds(5), true)) return 4;
    if (sink->Count() != 1) return 5;

    std::cout << LikesProgram::Logging::PackageName()
        << " consumer check passed\n";
    return 0;
}
