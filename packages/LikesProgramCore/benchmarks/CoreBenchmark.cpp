#ifdef _MSC_VER
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif

#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/StringView.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>
#include <LikesProgram/Core/time/Clock.hpp>
#include <unicode/Convert.hpp>

#include <chrono>
#include <codecvt>
#include <cstdint>
#include <format>
#include <iostream>
#include <locale>
#include <optional>
#include <span>
#include <string>
#include <array>

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

    void Print(const char* name, long long likesNs, long long stdNs) {
        std::cout << name
            << " likes_ns=" << likesNs
            << " std_ns=" << stdNs
            << std::endl;
    }

    // 统计 UTF-16 文本的 Unicode code point 数，作为 String::Size() 的 std 等价语义。
    LP_BENCH_NOINLINE size_t StdCodePointCount(std::u16string_view text) {
        size_t count = 0; // 已统计的 code point 数
        for (size_t i = 0; i < text.size();) {
            const char16_t c = text[i]; // 当前 UTF-16 code unit
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                i += 2;
            }
            else {
                ++i;
            }
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

    // std 长度 API 通过函数边界访问，匹配 Core 动态库 public API 的调用形态。
    LP_BENCH_NOINLINE size_t StdLengthApi(const std::u16string& text) {
        return text.size();
    }

    // std code point 缓存对照，模拟 String::Size() 首次扫描后命中缓存的语义。
    LP_BENCH_NOINLINE size_t StdCachedCodePointSizeApi(const std::optional<size_t>& cached, std::u16string_view text) {
        return cached.has_value() ? *cached : StdCodePointCount(text);
    }

    // std::format 增加异常边界和返回检查，匹配 String::Format 的稳定回退目标。
    LP_BENCH_NOINLINE std::u16string SafeStdFormatUtf16(int hexValue, int decValue, const wchar_t* text) {
        try {
            std::wstring formatted = std::format(L"{:#08X}-{:08d}-{}", hexValue, decValue, text); // std 格式化中间结果
            return std::u16string(reinterpret_cast<const char16_t*>(formatted.data()), formatted.size());
        }
        catch (...) {
            return u"{!}";
        }
    }

    // 从 std 字符串来源构造视图并读取，和 StringView(String) 一样包含来源对象访问。
    LP_BENCH_NOINLINE size_t StdStringViewFromStringApi(const std::u16string& text) {
        std::u16string_view view(text); // 不拥有的 std UTF-16 视图
        return view.size() + static_cast<size_t>(view.at(0));
    }

    // Result<int> 的 std 对照使用函数边界承载 optional 成功路径。
    LP_BENCH_NOINLINE int StdOptionalValueApi(int value) {
        std::optional<int> result(value); // std 成功路径对照对象
        return result.value();
    }
}

int main() {
    constexpr int iterations = 20000; // 长度/size 类微基准循环次数

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
    Print("append_length", likesAppendLength, stdAppendLength);

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
    Print("append_size", likesAppendSize, stdAppendSize);

    auto likesLength = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += largeLikes.Length() + Probe();
        return total;
    });
    auto stdLength = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += StdLengthApi(largeStd) + Probe();
        return total;
    });
    Print("length", likesLength, stdLength);

    auto likesSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += largeLikes.Size() + Probe();
        return total;
    });
    const std::optional<size_t> cachedStdCodePoints = StdCodePointCount(largeStd); // std 等价语义的预计算缓存
    auto stdSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < iterations; ++i) total += StdCachedCodePointSizeApi(cachedStdCodePoints, largeStd) + Probe();
        return total;
    });
    Print("size", likesSize, stdSize);

    auto likesFind = MeasureNs([&] {
        std::uint64_t total = 0;
        LikesProgram::String needle(U"gamma \U0001F600");
        for (int i = 0; i < 1000; ++i) total += largeLikes.Find(needle) + Probe();
        return total;
    });
    auto stdFind = MeasureNs([&] {
        std::uint64_t total = 0;
        std::u16string needle = u"gamma ";
        needle.push_back(0xD83D);
        needle.push_back(0xDE00);
        for (int i = 0; i < 1000; ++i) total += StdFindCodePoint(largeStd, needle) + Probe();
        return total;
    });
    Print("find", likesFind, stdFind);

    auto likesFormat = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < 2000; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, U"ok");
            total += s.Length() + Probe();
        }
        return total;
    });
    auto stdFormat = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < 2000; ++i) {
            const int hexValue = 255 + static_cast<int>(Probe() & 0);
            const int decValue = -42 - static_cast<int>(Probe() & 0);
            std::u16string s = SafeStdFormatUtf16(hexValue, decValue, L"ok");
            total += s.size() + Probe();
        }
        return total;
    });
    Print("format_smoke", likesFormat, stdFormat);

    std::u32string_view okView = U"ok";
    auto likesFormatView = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < 2000; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, okView);
            total += s.Length() + Probe();
        }
        return total;
    });
    Print("format_view", likesFormatView, stdFormat);

    auto likesFormatSize = MeasureNs([&] {
        std::uint64_t total = 0;
        for (int i = 0; i < 2000; ++i) {
            auto s = LikesProgram::String::Format(U"{:#08X}-{:08d}-{}", 255, -42, U"ok");
            total += s.Size() + Probe();
        }
        return total;
    });
    Print("format_size", likesFormatSize, stdFormat);

    auto likesUtf = MeasureNs([&] {
        std::uint64_t total = 0;
        auto utf8 = largeLikes.ToStdString();
        for (int i = 0; i < 200; ++i) {
            auto utf16 = LikesProgram::Unicode::Convert::Utf8ToUtf16(
                std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),
                    reinterpret_cast<const char8_t*>(utf8.data() + utf8.size())));
            total += utf16.size() + Probe();
        }
        return total;
    });
    auto stdUtf = MeasureNs([&] {
        std::uint64_t total = 0;
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        auto utf8 = largeLikes.ToStdString();
        for (int i = 0; i < 200; ++i) {
            auto wide = converter.from_bytes(utf8);
            total += wide.size() + Probe();
        }
        return total;
    });
    Print("utf8_to_utf16", likesUtf, stdUtf);

    // 公共契约层基准：薄封装应接近 std 视图/chrono/optional 的开销。
    constexpr int contractIterations = 200000; // 契约层微基准循环次数
    LikesProgram::String contractText(U"contract-view-\U0001F600"); // StringView 来源文本
    std::u16string stdContractText = contractText.ToU16String();     // std::u16string_view 来源文本

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
            total += StdStringViewFromStringApi(stdContractText) + Probe();
        }
        return total;
    });
    Print("contract_string_view", likesStringView, stdStringView);

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
    Print("contract_string_view_raw", likesStringViewRaw, stdStringViewRaw);

    std::array<std::byte, 256> byteStorage{}; // ByteSpan 与 std::span 共享样本
    for (size_t i = 0; i < byteStorage.size(); ++i) {
        byteStorage[i] = static_cast<std::byte>(i & 0xFF);
    }

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
            auto part = bytes.subspan(32, 64);
            total += part.size() + std::to_integer<std::uint64_t>(part[0]) + Probe();
        }
        return total;
    });
    Print("contract_byte_span", likesByteSpan, stdByteSpan);

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
        for (int i = 0; i < contractIterations; ++i) {
            total += static_cast<std::uint64_t>(StdOptionalValueApi(i)) + Probe();
        }
        return total;
    });
    Print("contract_result", likesResult, stdOptional);

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
    Print("contract_deadline", likesDeadline, stdDeadline);

    return 0;
}
