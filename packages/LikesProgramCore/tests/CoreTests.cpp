#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/StringView.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <LikesProgram/Core/system/Platform.hpp>
#include <LikesProgram/Core/system/ScopeGuard.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>
#include <LikesProgram/Core/time/Clock.hpp>
#include <stringFormat/FormatInternal.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <LikesProgram/Core/time/Timer.hpp>
#include <unicode/Convert.hpp>

#include <chrono>
#include <cctype>
#include <iostream>
#include <atomic>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

// Core 回归测试覆盖 String、unicode、stringFormat、time 与 Timer 的稳定契约。
namespace {
    // 测试助手保持轻量，失败时抛异常让 CTest 获取明确错误信息。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void RequireEq(const LikesProgram::String& actual, const LikesProgram::String& expected, const char* message) {
        if (actual != expected) {
            throw std::runtime_error(std::string(message) + ": actual=\"" +
                actual.ToStdString() + "\" expected=\"" + expected.ToStdString() + "\"");
        }
    }

    template<typename F>
    void RequireThrows(F&& fn, const char* message) {
        try {
            fn();
        }
        catch (const std::exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    std::u8string U8Bytes(std::initializer_list<unsigned char> bytes) {
        std::u8string result; // 构造包含非法字节序列的 UTF-8 测试输入
        for (unsigned char b : bytes) result.push_back(static_cast<char8_t>(b));
        return result;
    }

    // String 测试覆盖 code point 长度、moved-from 可用性、Split 与缓存语义。
    void TestString() {
        LikesProgram::String hello(U"Hello \u4E16\u754C"); // BMP 混合文本样本
        Require(hello.Size() == 8, "String Size should count Unicode code points");
        Require(hello.Length() == 8, "String Length should count UTF-16 code units for BMP text");
        Require(hello.StartsWith(u8"Hello"), "String StartsWith failed");
        Require(hello.EndsWith(U"\u4E16\u754C"), "String EndsWith failed");
        Require(hello.ToUpper().StartsWith(u8"HELLO"), "String ToUpper failed");

        LikesProgram::String movedSource(U"move-me"); // move 后仍需保持可复用的源对象
        LikesProgram::String movedTarget(std::move(movedSource)); // move 构造目标对象
        RequireEq(movedTarget, U"move-me", "String move target mismatch");
        Require(movedSource.Empty(), "moved-from String should be empty");
        Require(movedSource.Size() == 0, "moved-from String Size should be zero");
        Require(movedSource.ToStdString().empty(), "moved-from String should convert to empty UTF-8");
        movedSource.Append(U"reuse");
        RequireEq(movedSource, U"reuse", "moved-from String should be reusable");

        LikesProgram::String text(U"ababa"); // 重叠查找样本
        Require(text.Find(U"aba") == 0, "String Find overlap failed");
        Require(text.LastFind(U"aba", LikesProgram::String::npos) == 2, "String LastFind overlap failed");
        Require(text.LastFind(U"aba", 1) == 0, "String LastFind start bound failed");
        Require(text.LastFind(U"missing") == LikesProgram::String::npos, "String LastFind missing failed");

        LikesProgram::String splitText(U"A\U0001F600B\U0001F600C"); // 非 BMP 分隔符样本
        Require(splitText.Size() == 5, "String Size should count non-BMP as one code point");
        Require(splitText.Length() == 7, "String Length should count non-BMP as two UTF-16 code units");
        auto parts = splitText.Split(U"\U0001F600"); // Split 返回的三段文本
        Require(parts.size() == 3, "String Split non-BMP separator count failed");
        RequireEq(parts[0], U"A", "String Split non-BMP first part failed");
        RequireEq(parts[1], U"B", "String Split non-BMP second part failed");
        RequireEq(parts[2], U"C", "String Split non-BMP third part failed");

        LikesProgram::String large; // 用于验证 Size() 缓存的大字符串
        for (int i = 0; i < 200; ++i) large.Append(U"abc\U0001F600");
        Require(large.Size() == 800, "String cached Size failed");
        Require(large.Size() == 800, "String cached Size repeat failed");
        Require(large.Find(U"bc\U0001F600a") == 1, "String streaming Find failed");

        LikesProgram::String selfAppend(U"ab\U0001F600");
        selfAppend.Append(selfAppend);
        RequireEq(selfAppend, U"ab\U0001F600ab\U0001F600", "String self Append failed");
        selfAppend.Append(std::u16string_view(selfAppend.data(), 2));
        RequireEq(selfAppend.Right(2), U"ab", "String Append u16string_view failed");

        selfAppend.Clear();
        Require(selfAppend.Empty(), "String Clear should leave string empty");
        for (int i = 0; i < 128; ++i) selfAppend.Append(U"x");
        Require(selfAppend.Size() == 128, "String Append after Clear failed");

        LikesProgram::String stdCompat = std::string_view("std");
        stdCompat = std::u16string_view(u"hello\0unit", 10);
        Require(stdCompat.Length() == 10, "String std::u16string_view should preserve embedded NUL code units");
        Require(stdCompat.c_str()[5] == u'\0', "String c_str should expose UTF-16 storage");
        std::u16string asU16 = stdCompat;
        Require(asU16.size() == 10, "String conversion to std::u16string failed");
        stdCompat = std::wstring_view(L"wide");
        std::wstring asWide = stdCompat;
        Require(asWide == L"wide", "String conversion to std::wstring failed");
        std::string asUtf8 = LikesProgram::String(u8"utf8");
        Require(asUtf8 == "utf8", "String conversion to std::string failed");
        LikesProgram::String fromAny;
        Require(LikesProgram::String::FromAny(std::string_view("any"), fromAny), "String FromAny string_view failed");
        RequireEq(fromAny, U"any", "String FromAny string_view value failed");

        std::atomic<bool> failed{ false };
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&] {
                for (int j = 0; j < 100; ++j) {
                    if (large.Size() != 800) failed.store(true);
                    if (large.At(3) != U'\U0001F600') failed.store(true);
                }
            });
        }
        for (auto& reader : readers) reader.join();
        Require(!failed.load(), "String concurrent const cache read failed");
    }

    void TestUnicode() {
        auto utf16 = LikesProgram::Unicode::Convert::Utf8ToUtf16(u8"Likes");
        auto utf8 = LikesProgram::Unicode::Convert::Utf16ToUtf8(utf16);
        Require(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) == "Likes",
            "Unicode round trip failed");

        auto nonBmpUtf16 = LikesProgram::Unicode::Convert::Utf32ToUtf16(U"\U0001F600");
        auto nonBmpUtf8 = LikesProgram::Unicode::Convert::Utf16ToUtf8(nonBmpUtf16);
        auto nonBmpUtf32 = LikesProgram::Unicode::Convert::Utf16ToUtf32(nonBmpUtf16);
        Require(nonBmpUtf32.size() == 1 && nonBmpUtf32[0] == U'\U0001F600', "Unicode non-BMP round trip failed");
        Require(!nonBmpUtf8.empty(), "Unicode non-BMP UTF-8 output should not be empty");

        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xC0, 0xAF })); },
            "Unicode should reject overlong UTF-8");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xE0, 0x80, 0x80 })); },
            "Unicode should reject overlong three-byte UTF-8");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xED, 0xA0, 0x80 })); },
            "Unicode should reject UTF-8 encoded surrogate");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xF4, 0x90, 0x80, 0x80 })); },
            "Unicode should reject UTF-8 code points above U+10FFFF");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xE2, 0x82 })); },
            "Unicode should reject truncated UTF-8");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf8ToUtf16(U8Bytes({ 0xE2, 0x28, 0xA1 })); },
            "Unicode should reject invalid UTF-8 continuation");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf16ToUtf8(std::u16string{ 0xD800 }); },
            "Unicode should reject dangling UTF-16 high surrogate");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf16ToUtf32(std::u16string{ 0xDC00 }); },
            "Unicode should reject dangling UTF-16 low surrogate");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf32ToUtf16(std::u32string{ 0xD800 }); },
            "Unicode should reject UTF-32 surrogate code point");
        RequireThrows([] { LikesProgram::Unicode::Convert::Utf32ToUtf16(std::u32string{ 0x110000 }); },
            "Unicode should reject UTF-32 code point above U+10FFFF");
    }

    void TestFormat() {
        LikesProgram::String text = LikesProgram::String::Format(u"{} {:04d} {:#x}", u"Core", 7, 255);
        RequireEq(text, U"Core 0007 0xff", "String::Format failed");

        RequireEq(LikesProgram::String::Format(U"Hello, {}!", U"World"), U"Hello, World!",
            "String::Format basic automatic index failed");
        RequireEq(LikesProgram::String::Format(U"{1} + {0} = {2}", 2, 3, 5), U"3 + 2 = 5",
            "String::Format explicit index failed");
        RequireEq(LikesProgram::String::Format(U"{2}{}{}{}", 1, 2, 3, 4, 5), U"3124",
            "String::Format mixed index failed");
        RequireEq(LikesProgram::String::Format(U"{1}{}{1}{}{}", U"A", U"B", U"C", U"D", U"E"), U"BABCD",
            "String::Format repeated mixed index failed");
        RequireEq(LikesProgram::String::Format(U"{:*>8}", 42), U"******42",
            "String::Format right fill failed");
        RequireEq(LikesProgram::String::Format(U"{:'--'<10}", U"abc"), U"abc-------",
            "String::Format multi-code-point fill should be clipped to width");
        RequireEq(LikesProgram::String::Format(U"{:'ab'>5}", U"x"), U"ababx",
            "String::Format multi-code-point left padding failed");
        RequireEq(LikesProgram::String::Format(U"{:#08X}", 255), U"0X0000FF",
            "String::Format alternate uppercase hex zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:#08x}", 255), U"0x0000ff",
            "String::Format alternate lowercase hex zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:08d}", -42), U"-0000042",
            "String::Format signed decimal zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:+d} {: d}", 42, 42), U"+42  42",
            "String::Format sign control failed");
        RequireEq(LikesProgram::String::Format(U"{:.3f}", 3.14159), U"3.142",
            "String::Format floating precision failed");
        RequireEq(LikesProgram::String::Format(U"{:.3s}", U"\u4F60\u597Dabc"), U"\u4F60\u597Da",
            "String::Format string precision failed");
        RequireEq(LikesProgram::String::Format(U"{:.2s}", U"A\U0001F600B"), U"A\U0001F600",
            "String::Format string precision should preserve surrogate pairs");
        RequireEq(LikesProgram::String::Format(U"{} {}", std::string_view("sv"), std::u16string_view(u"u16")),
            U"sv u16", "String::Format std string_view arguments failed");
        const char32_t* formatPtr = U"ptr";
        const char32_t* nullU32Ptr = nullptr;
        RequireEq(LikesProgram::String::Format(U"{} {} {}", U"lit", formatPtr, std::u32string_view(U"view")),
            U"lit ptr view", "String::Format UTF-32 literal/pointer/view arguments failed");
        RequireEq(LikesProgram::String::Format(U"{}", nullU32Ptr), U"",
            "String::Format null UTF-32 pointer should format as empty string");
        RequireEq(LikesProgram::String::Format(U"{{}}"), U"{}",
            "String::Format brace escaping failed");
        RequireEq(LikesProgram::String::Format(U"{99}", U"x"), U"{?}",
            "String::Format out-of-range index fallback failed");
        RequireEq(LikesProgram::String::Format(U"{:unknown}", U"x"), U"{!type}",
            "String::Format unknown named formatter fallback failed");
        RequireEq(LikesProgram::String::Format(U"{"), U"{!}",
            "String::Format unmatched brace fallback failed");

        auto& internal = LikesProgram::StringFormat::FormatInternal::Instance();
        internal.RegisterFormatter("phase15_reentrant", [&internal](const LikesProgram::Any&, const LikesProgram::StringFormat::FormatSpec&) {
            internal.RegisterFormatter("phase15_nested", [](const LikesProgram::Any&, const LikesProgram::StringFormat::FormatSpec&) {
                return LikesProgram::String(U"nested");
            });
            return LikesProgram::String(U"ok");
        });
        RequireEq(LikesProgram::String::Format(U"{:uphase15_reentrant}", 1), U"ok",
            "String::Format named formatter should not run under registry lock");
        Require(internal.HasFormatter("phase15_nested"), "String::Format named formatter callback side effect failed");
        internal.UnregisterFormatter("phase15_reentrant");
        internal.UnregisterFormatter("phase15_nested");
    }

    void TestTime() {
        auto point = LikesProgram::Time::NsToSystemClock(LikesProgram::Time::Nanoseconds(123456700));
        auto text = LikesProgram::Time::FormatTime(point, u"%Y");
        Require(text.Length() == 4, "FormatTime year length failed");

        RequireEq(LikesProgram::Time::FormatTime(point, U"%3f %6f %9f"), U"123 123456 123456700",
            "FormatTime fractional seconds failed");
        auto apText = LikesProgram::Time::FormatTime(point, U"AP ap").ToStdString();
        Require(apText == "AM am" || apText == "PM pm", "FormatTime AP/ap casing failed");

        auto beforeEpoch = LikesProgram::Time::NsToSystemClock(LikesProgram::Time::Nanoseconds(-750000000));
        RequireEq(LikesProgram::Time::FormatTime(beforeEpoch, U"%9f"), U"250000000",
            "FormatTime negative time point fractional seconds failed");
    }

    void TestTimer() {
        LikesProgram::Time::Timer parent;
        LikesProgram::Time::Timer timer(false, &parent);
        Require(timer.Stop().count() == 0, "Timer Stop before Start should be zero");
        timer.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = timer.Stop();

        Require(elapsed.count() > 0, "Timer elapsed should be positive");
        Require(timer.GetLastElapsed().count() == elapsed.count(), "Timer last elapsed mismatch");
        Require(parent.GetAccumulatedElapsed().count() >= elapsed.count(), "Timer parent accumulation failed");
        Require(timer.Stop().count() == 0, "Timer repeated Stop should be zero");
        timer.Reset();
        Require(timer.GetLastElapsed().count() == 0, "Timer Reset last elapsed failed");
        Require(timer.GetAccumulatedElapsed().count() == 0, "Timer Reset accumulated elapsed failed");

        LikesProgram::Time::Timer running(true);
        LikesProgram::Time::Timer copied(running);
        Require(!copied.IsRunning(), "Timer copy should not preserve running state");
        running.Stop();
    }

    // Core 契约层测试覆盖后续扩展包会优先复用的轻量公共能力。
    void TestCoreContracts() {
        auto version = LikesProgram::Version::Current(); // 当前统一版本信息
        Require(version.major == 1 && version.minor == 0 && version.patch == 0,
            "Version current tuple failed");
        Require(LikesProgram::Version::IsAtLeast(1, 0, 0), "Version IsAtLeast failed");
        Require(LikesProgram::Version::CurrentString() == std::string_view("1.0.0"),
            "Version string failed");

        LikesProgram::Status ok; // 默认状态表示成功
        Require(ok.IsOk(), "Status default should be ok");
        auto invalid = LikesProgram::Status::InvalidArgument(U"bad input");
        Require(!invalid.IsOk(), "Status invalid argument should fail");
        Require(invalid.Code() == LikesProgram::StatusCode::InvalidArgument,
            "Status code mismatch");
        Require(invalid.ToString().StartsWith(U"InvalidArgument"),
            "Status ToString should include code name");

        LikesProgram::Result<int> value(42); // 成功结果携带值
        Require(value.IsOk() && value.Value() == 42, "Result value failed");
        LikesProgram::Result<int> failed(invalid);
        Require(!failed.IsOk(), "Result failed status should not be ok");
        Require(failed.ValueOr(7) == 7, "Result ValueOr failed");
        RequireThrows([&] { (void)failed.Value(); }, "Result Value should throw on failure");
        LikesProgram::Result<void> voidResult(LikesProgram::Status::OkStatus());
        Require(voidResult.IsOk(), "Result<void> ok failed");
        LikesProgram::Result<LikesProgram::String> textResult(LikesProgram::String(U"copy"));
        LikesProgram::Result<LikesProgram::String> copiedText(textResult);
        RequireEq(copiedText.Value(), U"copy", "Result copy constructor failed");
        LikesProgram::Result<LikesProgram::String> movedText(std::move(copiedText));
        RequireEq(movedText.Value(), U"copy", "Result move constructor failed");
        LikesProgram::Result<LikesProgram::String> assigned(LikesProgram::Status::NotFound(U"missing"));
        assigned = movedText;
        RequireEq(assigned.Value(), U"copy", "Result assignment from value branch failed");
        assigned = LikesProgram::Result<LikesProgram::String>(LikesProgram::Status::NotFound(U"missing"));
        Require(!assigned.IsOk() && assigned.GetStatus().Code() == LikesProgram::StatusCode::NotFound,
            "Result assignment from status branch failed");

        LikesProgram::String owned(U"view"); // StringView 不拥有该字符串
        LikesProgram::StringView view(owned);
        Require(view.Length() == owned.Length(), "StringView length failed");
        Require(view.At(0) == u'v', "StringView At failed");
        RequireEq(view.ToString(), owned, "StringView ToString failed");
        LikesProgram::StringView nullView(static_cast<const char16_t*>(nullptr));
        Require(nullView.Empty(), "StringView null should be empty");

        std::array<std::byte, 4> bytes{
            std::byte{ 0x01 }, std::byte{ 0x2A }, std::byte{ 0xFF }, std::byte{ 0x00 }
        };
        LikesProgram::ByteSpan span(bytes.data(), bytes.size());
        Require(span.Size() == 4, "ByteSpan size failed");
        span.SubSpan(1, 2).Fill(std::byte{ 0x0F });
        Require(std::to_integer<int>(bytes[1]) == 15 && std::to_integer<int>(bytes[2]) == 15,
            "ByteSpan Fill failed");
        RequireEq(span.AsConst().ToHexString(true), U"010F0F00", "ByteSpan hex failed");
        Require(span.AsConst().SubSpan(4).Empty(), "ByteSpan empty tail failed");
        RequireThrows([&] { (void)span.SubSpan(5); }, "ByteSpan out-of-range should throw");

        int guardValue = 0; // ScopeGuard 退出时递增该值
        {
            auto guard = LikesProgram::MakeScopeGuard([&] { guardValue += 1; });
        }
        Require(guardValue == 1, "ScopeGuard should run callback");
        {
            auto guard = LikesProgram::MakeScopeGuard([&] { guardValue += 10; });
            guard.Dismiss();
        }
        Require(guardValue == 1, "ScopeGuard Dismiss failed");

        auto begin = LikesProgram::Time::Clock::Now(); // Clock 使用单调时钟
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(LikesProgram::Time::Clock::Since(begin).count() > 0, "Clock Since failed");
        auto deadline = LikesProgram::Time::Deadline::FromNow(std::chrono::milliseconds(5));
        Require(deadline.HasDeadline(), "Deadline should have target");
        Require(!deadline.Expired(), "Deadline should not expire immediately");
        Require(deadline.Remaining().count() > 0, "Deadline remaining failed");
        auto infinite = LikesProgram::Time::Deadline::Infinite();
        Require(!infinite.HasDeadline(), "Infinite deadline should not have target");
        Require(infinite.Remaining() == LikesProgram::Time::Duration::max(),
            "Infinite deadline remaining failed");

        Require(!LikesProgram::Platform::OperatingSystemName().empty(),
            "Platform OS name should not be empty");
        Require(!LikesProgram::Platform::ArchitectureName().empty(),
            "Platform architecture name should not be empty");
        Require(!LikesProgram::Platform::CompilerName().empty(),
            "Platform compiler name should not be empty");
        static_assert(LikesProgram::Platform::CxxStandard >= 202002L,
            "Platform C++ standard should be C++20 or newer");
    }
}

int main() {
    try {
        TestString();
        TestUnicode();
        TestFormat();
        TestTime();
        TestTimer();
        TestCoreContracts();
    }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
