#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/StringView.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Core 压力测试固定覆盖随机输入、极值、并发只读和对象生命周期。
namespace {
    // 断言失败时抛出异常，让 CTest 获得稳定失败信息。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 小型确定性生成器保证所有平台复现相同样本序列。
    std::uint32_t NextRandom(std::uint32_t& state) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // 随机格式串允许失败回退，但不得抛异常、越界或无限循环。
    void TestFormatFuzz() {
        constexpr std::array<char16_t, 24> alphabet{
            u'{', u'}', u':', u'0', u'1', u'2', u'9', u'#', u'+', u'-', u' ', u'.',
            u'<', u'>', u'^', u'=', u'd', u'x', u'f', u's', u'u', u'\'', u'"', u'A'
        };
        std::uint32_t state = 0xC0DEF00Du; // 固定 fuzz 种子
        for (int sample = 0; sample < 10000; ++sample) {
            const size_t length = NextRandom(state) % 65; // 单样本最多 64 个 UTF-16 单元
            std::u16string format; // 当前随机格式串
            format.reserve(length);
            for (size_t i = 0; i < length; ++i) format.push_back(alphabet[NextRandom(state) % alphabet.size()]);

            LikesProgram::String output; // 格式化回退结果
            try {
                output = LikesProgram::String::Format(std::u16string_view(format), 42, -7, U"payload", 3.25);
            } catch (...) {
                throw std::runtime_error("String::Format fuzz input escaped public exception boundary");
            }
            Require(output.Length() < 1024 * 1024, "String::Format fuzz output grew without bound");
        }
    }

    // 随机 UTF-8 字节要么严格拒绝，要么可无损往返。
    void TestUtf8Fuzz() {
        std::uint32_t state = 0x51A7E123u; // 固定 UTF-8 fuzz 种子
        for (int sample = 0; sample < 10000; ++sample) {
            const size_t length = NextRandom(state) % 33; // 单样本最多 32 字节
            std::u8string bytes; // 当前随机 UTF-8 字节序列
            bytes.reserve(length);
            for (size_t i = 0; i < length; ++i) bytes.push_back(static_cast<char8_t>(NextRandom(state) & 0xFFu));

            try {
                LikesProgram::String decoded(bytes); // 严格 UTF-8 解码结果
                const std::u8string encoded = decoded.ToU8String(); // 重新编码后的规范 UTF-8
                LikesProgram::String roundTrip(encoded); // 第二次解码必须稳定
                Require(roundTrip == decoded, "UTF-8 accepted input failed round trip");
            } catch (const std::runtime_error&) {
                // 非法随机序列按公开契约抛出 runtime_error。
            }
        }
    }

    // 大输入覆盖容量计算、查找、追加和编码往返的线性边界。
    void TestLargeInput() {
        constexpr size_t inputSize = 1024 * 1024; // 1 MiB ASCII 输入
        std::u8string bytes(inputSize, u8'a'); // 大输入 UTF-8 字节串
        bytes[inputSize - 4] = u8't';
        bytes[inputSize - 3] = u8'a';
        bytes[inputSize - 2] = u8'i';
        bytes[inputSize - 1] = u8'l';

        LikesProgram::String text(bytes); // 1 MiB UTF-8 转内部 UTF-16
        Require(text.Length() == inputSize, "large String UTF-16 length mismatch");
        Require(text.Size() == inputSize, "large String code point count mismatch");
        Require(text.Find(U"tail") == inputSize - 4, "large String tail find mismatch");
        Require(text.ToU8String() == bytes, "large String UTF-8 round trip mismatch");

        text.Append(U"-suffix");
        Require(text.EndsWith(U"-suffix"), "large String append suffix mismatch");
    }

    // 并发只读与线程本地格式缓存必须长期保持一致结果。
    void TestConcurrentReadAndFormat() {
        LikesProgram::String shared(U"alpha-beta-gamma-你好-\U0001F600"); // 多脚本共享只读文本
        const size_t sharedSize = shared.Size(); // 并发样本的 Unicode code point 数
        if (sharedSize != 21) throw std::runtime_error("concurrent sample code point count mismatch: " + std::to_string(sharedSize));
        std::atomic<bool> failed{ false }; // 任一 worker 的一致性失败标记
        std::vector<std::thread> workers; // 并发读与格式化 worker
        workers.reserve(8);

        for (int worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&, worker] {
                for (int iteration = 0; iteration < 2000; ++iteration) {
                    if (shared.Size() != 21 || shared.Find(U"gamma") != 11) failed.store(true);
                    LikesProgram::String formatted = LikesProgram::String::Format(U"worker={:02d};iteration={:04d};{}", worker, iteration, shared);
                    if (!formatted.EndsWith(shared)) failed.store(true);
                    if (shared.ToU8String().empty()) failed.store(true);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        Require(!failed.load(), "concurrent String read/format consistency failed");
    }

    // 高频复制、移动和轻量契约组合验证析构顺序与 moved-from 可用性。
    void TestLifecycleStress() {
        for (int iteration = 0; iteration < 5000; ++iteration) {
            LikesProgram::String source = LikesProgram::String::Format(U"item-{}-\U0001F600", iteration);
            LikesProgram::String copy(source); // 深拷贝路径
            LikesProgram::String moved(std::move(source)); // 移动构造路径
            Require(source.Empty(), "moved-from source should be empty");
            source = copy; // moved-from 复制赋值复用
            moved = std::move(source); // 移动赋值路径
            Require(source.Empty(), "move-assigned source should be empty");

            LikesProgram::Result<LikesProgram::String> result(moved); // Result 值分支生命周期
            LikesProgram::StringView view(result.Value()); // 非拥有视图生命周期
            Require(!view.Empty() && view.ToString() == moved, "Result/StringView lifecycle mismatch");

            std::array<std::byte, 16> storage{}; // ByteSpan 边界样本
            LikesProgram::ByteSpan span(storage);
            span.SubSpan(4, 8).Fill(static_cast<std::byte>(iteration & 0xFF));
            Require(span.Size() == storage.size(), "ByteSpan lifecycle size mismatch");
        }
    }
}

int main() {
    try {
        TestFormatFuzz();
        TestUtf8Fuzz();
        TestLargeInput();
        TestConcurrentReadAndFormat();
        TestLifecycleStress();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
