#include <LikesProgram/Net/Net.hpp>
#include <cstring>
#include <iostream>

namespace {
    class BackpressureAwareConnection final : public LikesProgram::Net::Connection {
    public:
        // 示例连接只展示公开背压钩子接法，不打开真实 socket。
        BackpressureAwareConnection(
            LikesProgram::Net::SocketType fd,
            LikesProgram::Net::EventLoop* loop)
            : Connection(fd, loop) {
        }

    protected:
        // 写队列达到高水位时暂停读，向上游施加背压。
        void OnWriteHighWatermark(std::size_t) override {
            PauseReading();
        }

        // 写队列回落到低水位时恢复读。
        void OnWriteLowWatermark(std::size_t) override {
            ResumeReading();
        }

        // 写队列超过硬上限时关闭慢连接。
        void OnWriteQueueOverflow(std::size_t) override {
            ForceClose();
        }
    };
}

int main() {
    LikesProgram::Net::Buffer buffer;
    std::uint8_t* writeBegin = buffer.PrepareWrite(3);
    std::memcpy(writeBegin, "net", 3);
    buffer.HasWritten(3);

    LikesProgram::Net::Address loopback("127.0.0.1", 0);
    BackpressureAwareConnection connection(LikesProgram::Net::kInvalidSocket, nullptr);
    connection.SetWriteWatermark(64 * 1024, 16 * 1024);
    connection.SetMaxPendingWriteBytes(4 * 1024 * 1024);

    // 公开 Poller 入口只返回稳定诊断名，不暴露平台后端类型。
    auto poller = LikesProgram::Net::CreateDefaultPoller(nullptr);
    const char* backend = poller ? poller->BackendName() : "unavailable";
    const auto completionStats = poller
        ? poller->GetCompletionStats()
        : LikesProgram::Net::CompletionStats{};

    std::cout << LikesProgram::Net::PackageName()
        << " " << LikesProgram::Net::PackageVersion()
        << " address=" << loopback.ToString()
        << " buffer=" << buffer.AsStringView()
        << " backpressure=enabled"
        << " backend=" << backend
        << " provided_buffers=" << completionStats.providedBufferCount
        << '\n';

    return 0;
}
