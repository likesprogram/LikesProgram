#include "net/DatagramBatchPolicy.hpp"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

static_assert(std::is_same_v<
    decltype(LikesProgram::Net::Internal::DatagramProvidedBufferCount(512U)),
    std::uint32_t>);
static_assert(LikesProgram::Net::Internal::ShouldRetryDatagramRead(false, 0));
static_assert(!LikesProgram::Net::Internal::ShouldRetryDatagramRead(true, 0));
static_assert(LikesProgram::Net::Internal::ShouldRetryDatagramRead(true, 1));

namespace {
    // contract 失败立即终止，保持平台无关的最小运行入口。
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    // 编译期覆盖正常乘法，确保 helper 返回精确 pool 字节数。
    constexpr bool DatagramBufferBytesNormalContract() noexcept {
        std::size_t bytes = 0; // 成功路径接收完整乘积
        return LikesProgram::Net::Internal::TryMultiplyDatagramBufferBytes(
            65680,
            64,
            bytes)
            && bytes == 4203520;
    }

    // 任一维度为零时乘积为零，仍属于合法计算。
    constexpr bool DatagramBufferBytesZeroContract() noexcept {
        std::size_t bytes = 1; // 零维度必须覆盖旧输出
        if (!LikesProgram::Net::Internal::TryMultiplyDatagramBufferBytes(0, 64, bytes)
            || bytes != 0) return false;
        bytes = 1;
        return LikesProgram::Net::Internal::TryMultiplyDatagramBufferBytes(65680, 0, bytes)
            && bytes == 0;
    }

    // 溢出路径不得执行乘法，并把输出收敛为零。
    constexpr bool DatagramBufferBytesOverflowContract() noexcept {
        std::size_t bytes = 1; // 失败路径不得保留误导性的旧值
        return !LikesProgram::Net::Internal::TryMultiplyDatagramBufferBytes(
            (std::numeric_limits<std::size_t>::max)(),
            2,
            bytes)
            && bytes == 0;
    }

    static_assert(DatagramBufferBytesNormalContract());
    static_assert(DatagramBufferBytesZeroContract());
    static_assert(DatagramBufferBytesOverflowContract());
}

int main() {
    using namespace LikesProgram::Net::Internal;

    Require(DatagramProvidedBufferSize(16, 128, 65536) == 65680);
    Require(DatagramProvidedBufferCount(512) == 64);
    Require(DatagramProvidedBufferCount(4096) == 256);

    Require(DatagramSendBatchSize(64, 32, false) == 16);
    Require(DatagramSendBatchSize(3, 32, false) == 3);
    Require(DatagramSendBatchSize(64, 1, false) == 1);
    Require(DatagramSendBatchSize(64, 32, true) == 0);

    // 接收结果同时约束批量上限、实际 buffer 容量和内核截断标记。
    const DatagramBatchReceiveResult full =
        InterpretDatagramBatchReceive(9, 9, 64, false);
    Require(full.payloadBytes == 9);
    Require(full.originalBytes == 9);
    Require(!full.truncated);

    const DatagramBatchReceiveResult limited =
        InterpretDatagramBatchReceive(9, 9, 4, false);
    Require(limited.payloadBytes == 4);
    Require(limited.originalBytes == 9);
    Require(limited.truncated);

    const DatagramBatchReceiveResult shortBuffer =
        InterpretDatagramBatchReceive(9, 6, 64, true);
    Require(shortBuffer.payloadBytes == 6);
    Require(shortBuffer.originalBytes == 9);
    Require(shortBuffer.truncated);

    const DatagramBatchReceiveResult empty =
        InterpretDatagramBatchReceive(0, 0, 64, false);
    Require(empty.payloadBytes == 0);
    Require(empty.originalBytes == 0);
    Require(!empty.truncated);
}
