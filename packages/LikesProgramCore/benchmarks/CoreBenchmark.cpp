#ifdef _MSC_VER
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif

#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/StringView.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>
#include <LikesProgram/Core/time/Clock.hpp>
#include "CoreBenchmarkReference.hpp"

#include <chrono>
#include <codecvt>
#include <cstdint>
#include <iostream>
#include <locale>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <array>
#include <atomic>
#include <cwchar>

#if defined(__has_include)
#if __has_include(<format>)
#include <format>
#endif
#endif

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#define LP_BENCH_HAS_STD_FORMAT 1
#else
#define LP_BENCH_HAS_STD_FORMAT 0
#endif

#ifdef _MSC_VER
#define LP_BENCH_NOINLINE __declspec(noinline)
#else
#define LP_BENCH_NOINLINE __attribute__((noinline))
#endif

// Core 热路径基准用于观察 LikesProgram::String 与 std 路径的相对变化。
namespace {
    volatile std::uint64_t g_probe = 0; // 防止编译器完全消除循环计算

    // 给基准循环注入不可折叠读取，保持测量路径真实。
    std::uint64_t Probe() {
        return g_probe;
    }

    template<typename F>
    long long MeasureNs(F&& fn) {
        auto begin = std::chrono::steady_clock::now(); // 测量起点
        volatile std::uint64_t sink = fn();            // 保存结果避免优化掉被测逻辑
        (void)sink;
        auto end = std::chrono::steady_clock::now();   // 测量终点
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }

    void Print(const char* name, long long likesNs, long long stdNs, int operations) {
        std::cout << name << " likes_ns=" << likesNs << " std_ns=" << stdNs << " operations=" << operations << std::endl;
    }

    // 统计 UTF-16 文本的 Unicode code point 数，作为 String::Size() 的 std 等价语义。
    LP_BENCH_NOINLINE size_t StdCodePointCount(std::u16string_view text) {
        size_t count = 0; // 已统计的 code point 数
        for (size_t i = 0; i < text.size();) {
            const char16_t c = text[i]; // 当前 UTF-16 code unit
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                i += 2;
            } else ++i;
            ++count;
        }
        return count;
    }

    // 将 UTF-16 offset 转为 code point index，作为 String::Find() 返回值的 std 等价语义。
    LP_BENCH_NOINLINE size_t StdCodePointIndexBefore(std::u16string_view text, size_t offset) {
        return StdCodePointCount(text.substr(0, offset));
    }

    // 复用标准库 find 后转换返回语义，避免把 code unit index 当作 code point index。
    LP_BENCH_NOINLINE size_t StdFindCodePoint(std::u16string_view text, std::u16string_view needle) {
        const size_t offset = text.find(needle); // std::u16string_view 查找出的 UTF-16 offset
        if (offset == std::u16string_view::npos) return std::u16string_view::npos;
        return StdCodePointIndexBefore(text, offset);
    }

    // Result<int> 的 std 对照使用函数边界承载 optional 成功路径。
    LP_BENCH_NOINLINE int StdOptionalValueApi(int value) {
        std::optional<int> result(value); // std 成功路径对照对象
        return result.value();
    }

    using StdFindFunction = size_t(*)(std::u16string_view, std::u16string_view);
    using StdOptionalFunction = int(*)(int);

    StdFindFunction volatile g_stdFindFunction = &StdFindCodePoint; // std 查找对照入口
    StdOptionalFunction volatile g_stdOptionalFunction = &StdOptionalValueApi; // std optional 对照入口
}

int main() {
    constexpr int iterations = 2000000; // 长度/size 类微基准循环次数
    constexpr int formatIterations = 20000; // 格式化样本循环次数，降低亚毫秒调度噪声

    std::cout << "benchmark_version=7"
        << " format_reference=" << (LP_BENCH_HAS_STD_FORMAT ? "std::format" : "std::swprintf")
        << " utf_reference=std::wstring_convert"
        << " string_view_source_reference=std_pimpl_matching_linkage"
        << " string_hot_path_reference=std_pimpl_matching_linkage"
        << " format_source_reference=std_matching_linkage"
        << " parity_epsilon_ns_per_op=0.75"
        << std::endl;

    // 构造包含非 BMP 字符的相同文本，分别给 LikesProgram 与 std 路径使用。
    LikesProgram::String likesText(U"alpha beta gamma \U0001F600 delta "); // LikesProgram 基准文本
    std::u16string stdText = u"alpha beta gamma ";                         // std UTF-16 基准文本
    stdText.push_back(0xD83D);
    stdText.push_back(0xDE00);
    stdText += u" delta ";

    LikesProgram::String largeLikes; // 放大后的 LikesProgram 查找/长度样本
    std::u16string largeStd;         // 放大后的 std 查找/长度样本
    for (int i = 0; i < 2000; ++i) {
        largeLikes.Append(likesText);
        largeStd += stdText;
    }
    largeLikes.Append(U"omega-\U0001F642"); // 只在尾部出现的远距离查找目标
    largeStd += u"omega-";
    largeStd.push_back(0xD83D);
    largeStd.push_back(0xDE42);
    using StdStringOwner = std::unique_ptr<LikesProgramCoreBenchmarkStdString, decltype(&LikesProgramCoreBenchmarkDestroyStdString)>;
    StdStringOwner largeStdOwner(
        LikesProgramCoreBenchmarkCreateStdString(largeStd.data(), largeStd.size()),
        &LikesProgramCoreBenchmarkDestroyStdString
    ); // shared/static 均匹配 Core 的 PImpl 调用形态

    // Append 基准分开测 Length/Size，避免 code point 统计掩盖追加成本。
    auto likesAppendLength = MeasureNs([&] {
        std::uint64_t total = 0; // 累积结果，防止循环被优化
        LikesProgram::String text; // 被测追加目标
        std::u16string_view chunk(likesText.data(), likesText.Length()); // 直接追加的 UTF-16 视图
        for (int i = 0; i < 20000; ++i) {
            text.Append(chunk);
            total += static_cast<std::uint64_t>(i) + Probe();
        }
        total += text.Length();
        return total;
    });
    auto stdAppendLength = MeasureNs([&] {
        std::uint64_t total = 0; // 累积结果，防止循环被优化
        std::u16string text;     // std 追加目标
        for (int i = 0; i < 20000; ++i) {
            text += stdText;
            total += static_cast<std::uint64_t>(i) + Probe();
        }
        total += text.size();
        return total;
    });
    Print("append_length", likesAppendLength, stdAppendLength, 20000);

    auto likesAppendSize = MeasureNs([&] {
        std::uint64_t total = 0;
        LikesProgram::String text;
        std::u16string_view chunk(likesText.data(), likesText.Length());
        for (int i = 0; i < 20000; ++i) {
            text.Append(chunk);
            total += static_cast<std::uint64_t>(i) + Probe();
        }
        total += text.Size();
        return total;
    });
    auto stdAppendSize = MeasureNs([&] {
        std::uint64_t total = 0; // std 对照路径也统计 Unicode code point 数
        std::u16string text;
        for (int i = 0; i < 20000; ++i) {
            text += stdText;
            total += static_cast<std::uint64_t>(i) + Probe();
        }
        total += StdCodePointCount(text);
        return total;
    });
    Print("append_size", likesAppendSize, stdAppendSize, 20000);

    auto likesLength = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += largeLikes.Length() + Probe();
        return total;
    });
    auto stdLength = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += LikesProgramCoreBenchmarkStdStringLength(largeStdOwner.get()) + Probe();
        return total;
    });
    Print("length", likesLength, stdLength, iterations);

    auto likesSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += largeLikes.Size() + Probe();
        return total;
    });
    auto stdSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += LikesProgramCoreBenchmarkStdStringSize(largeStdOwner.get()) + Probe();
        return total;
    });
    Print("size", likesSize, stdSize, iterations);

    auto likesFind = MeasureNs([&] {
        std::uint64_t total = 0;
        LikesProgram::String needle(U"omega-\U0001F642");
        for (int i = 0; i < 1000; ++i) total += largeLikes.Find(needle) + Probe();
        return total;
    });
    auto stdFind = MeasureNs([&] {
        std::uint64_t total = 0;
        std::u16string needle = u"omega-";
        needle.push_back(0xD83D);
        needle.push_back(0xDE42);
        for (int i = 0; i < 1000; ++i) total += g_stdFindFunction(largeStd, needle) + Probe();
        return total;
    });
    Print("find", likesFind, stdFind, 1000);

    auto likesFormat = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < formatIterations; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, U"ok");
            total += s.Length() + Probe();
        }
        return total;
    });
    auto stdFormat = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < formatIterations; ++i) {
            const int hexValue = 255 + static_cast<int>(Probe() & 0);
            const int decValue = -42 - static_cast<int>(Probe() & 0);
            std::u16string s = LikesProgramCoreBenchmarkFormatStdString(hexValue, decValue, L"ok");
            total += s.size() + Probe();
        }
        return total;
    });
    Print("format_smoke", likesFormat, stdFormat, formatIterations);

    std::u32string_view okView = U"ok";
    auto likesFormatView = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < formatIterations; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, okView);
            total += s.Length() + Probe();
        }
        return total;
    });
    Print("format_view", likesFormatView, stdFormat, formatIterations);

    auto likesFormatSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < formatIterations; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, U"ok");
            total += s.Size() + Probe();
        }
        return total;
    });
    auto stdFormatSize = MeasureNs([&] {
        std::uint64_t total = 0; // std 输出按相同 Unicode code point 语义计数
        for (int i = 0; i < formatIterations; ++i) {
            const int hexValue = 255 + static_cast<int>(Probe() & 0);
            const int decValue = -42 - static_cast<int>(Probe() & 0);
            std::u16string s = LikesProgramCoreBenchmarkFormatStdString(hexValue, decValue, L"ok");
            total += StdCodePointCount(s) + Probe();
        }
        return total;
    });
    Print("format_size", likesFormatSize, stdFormatSize, formatIterations);

    auto likesUtf = MeasureNs([&] {
        std::uint64_t total = 0;
        auto utf8 = largeLikes.ToStdString();
        for (int i = 0; i < 200; ++i) {
            LikesProgram::String utf16Text(std::string_view(utf8.data(), utf8.size())); // 公开 UTF-8 构造覆盖内部 UTF-16 转换
            total += utf16Text.Length() + Probe();
        }
        return total;
    });
    auto stdUtf = MeasureNs([&] {
        std::uint64_t total = 0;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        auto utf8 = largeLikes.ToStdString();
        for (int i = 0; i < 200; ++i) {
            auto wide = converter.from_bytes(utf8);
            total += wide.size() + Probe();
        }
        return total;
    });
    Print("utf8_to_utf16", likesUtf, stdUtf, 200);

    // 公共契约层基准：薄封装应接近 std 视图/chrono/optional 的开销。
    constexpr int contractIterations = 2000000; // 契约层微基准循环次数，避免亚毫秒调度噪声
    LikesProgram::String contractText(U"contract-view-\U0001F600"); // StringView 来源文本
    std::u16string stdContractText = contractText.ToU16String();     // std::u16string_view 来源文本
    StdStringOwner stdContractTextOwner(
        LikesProgramCoreBenchmarkCreateStdString(stdContractText.data(), stdContractText.size()),
        &LikesProgramCoreBenchmarkDestroyStdString
    ); // 同链接形态的 std PImpl 来源对象

    auto likesStringView = MeasureNs([&] {
        std::uint64_t total = 0; // 累积长度和首字符，避免循环被消除
        for (int i = 0; i < contractIterations; ++i) {
            LikesProgram::StringView view(contractText);
            total += view.Length() + static_cast<std::uint64_t>(view.At(0)) + Probe();
        }
        return total;
    });
    auto stdStringView = MeasureNs([&] {
        std::uint64_t total = 0; // std 视图对照路径
        for (int i = 0; i < contractIterations; ++i) {
            std::u16string_view view; // 与导入构造函数相同，由外部入口写回两个视图字段
            LikesProgramCoreBenchmarkConstructStdStringView(&view, stdContractTextOwner.get());
            total += view.size() + static_cast<std::uint64_t>(view.at(0)) + Probe();
        }
        return total;
    });
    Print("contract_string_view", likesStringView, stdStringView, contractIterations);

    auto likesStringViewRaw = MeasureNs([&] {
        std::uint64_t total = 0; // 只测 StringView 本体，不混入 String 成员函数成本
        const char16_t* data = stdContractText.data(); // std 样本底层 UTF-16 指针
        size_t length = stdContractText.size();        // std 样本 UTF-16 code unit 数
        for (int i = 0; i < contractIterations; ++i) {
            LikesProgram::StringView view(data, length);
            total += view.Length() + static_cast<std::uint64_t>(view.At(0)) + Probe();
        }
        return total;
    });
    auto stdStringViewRaw = MeasureNs([&] {
        std::uint64_t total = 0; // std 直接指针/长度视图对照
        const char16_t* data = stdContractText.data();
        size_t length = stdContractText.size();
        for (int i = 0; i < contractIterations; ++i) {
            std::u16string_view view(data, length);
            total += view.size() + static_cast<std::uint64_t>(view.at(0)) + Probe();
        }
        return total;
    });
    Print("contract_string_view_raw", likesStringViewRaw, stdStringViewRaw, contractIterations);

    std::array<std::byte, 256> byteStorage{}; // ByteSpan 与 std::span 共享样本
    for (size_t i = 0; i < byteStorage.size(); ++i) byteStorage[i] = static_cast<std::byte>(i & 0xFF);

    auto likesByteSpan = MeasureNs([&] {
        std::uint64_t total = 0; // 累积子视图大小和字节值
        for (int i = 0; i < contractIterations; ++i) {
            LikesProgram::ByteSpan bytes(byteStorage.data(), byteStorage.size());
            auto part = bytes.SubSpan(32, 64);
            total += part.Size() + std::to_integer<std::uint64_t>(part[0]) + Probe();
        }
        return total;
    });
    auto stdByteSpan = MeasureNs([&] {
        std::uint64_t total = 0; // std::span 对照路径
        for (int i = 0; i < contractIterations; ++i) {
            std::span<std::byte> bytes(byteStorage);
            if (32 > bytes.size() || 64 > bytes.size() - 32) throw std::out_of_range("std::span benchmark range out of bounds");
            auto part = bytes.subspan(32, 64);
            total += part.size() + std::to_integer<std::uint64_t>(part[0]) + Probe();
        }
        return total;
    });
    Print("contract_byte_span", likesByteSpan, stdByteSpan, contractIterations);

    auto likesResult = MeasureNs([&] {
        std::uint64_t total = 0; // Result 成功路径读取成本
        for (int i = 0; i < contractIterations; ++i) {
            LikesProgram::Result<int> value(i);
            total += static_cast<std::uint64_t>(value.Value()) + Probe();
        }
        return total;
    });
    auto stdOptional = MeasureNs([&] {
        std::uint64_t total = 0; // std::optional 成功路径对照
        for (int i = 0; i < contractIterations; ++i) total += static_cast<std::uint64_t>(g_stdOptionalFunction(i)) + Probe();
        return total;
    });
    Print("contract_result", likesResult, stdOptional, contractIterations);

    auto likesDeadline = MeasureNs([&] {
        std::uint64_t total = 0; // Deadline 剩余时间查询成本
        for (int i = 0; i < contractIterations; ++i) {
            auto deadline = LikesProgram::Time::Deadline::FromNow(std::chrono::seconds(1));
            total += static_cast<std::uint64_t>(deadline.HasDeadline()) + Probe();
        }
        return total;
    });
    auto stdDeadline = MeasureNs([&] {
        std::uint64_t total = 0; // chrono 直接构造对照
        for (int i = 0; i < contractIterations; ++i) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            total += static_cast<std::uint64_t>(deadline.time_since_epoch().count() != 0) + Probe();
        }
        return total;
    });
    Print("contract_deadline", likesDeadline, stdDeadline, contractIterations);

    return 0;
}
