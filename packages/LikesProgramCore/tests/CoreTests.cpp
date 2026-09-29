#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/StringView.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <LikesProgram/Core/system/Platform.hpp>
#include <LikesProgram/Core/system/ScopeGuard.hpp>
#include <LikesProgram/Core/time/Deadline.hpp>
#include <LikesProgram/Core/time/Clock.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <LikesProgram/Core/time/Timer.hpp>

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

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

// Core 回归测试覆盖 String、unicode、stringFormat、time 与 Timer 的稳定契约。
namespace {
    std::atomic<int> g_lazyStatusFactoryCalls{ 0 }; // 延迟状态工厂实际物化次数

    // 测试助手保持轻量，失败时抛异常让 CTest 获取明确错误信息。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void RequireEq(const LikesProgram::String& actual, const LikesProgram::String& expected, const char* message) {
        if (actual != expected) throw std::runtime_error(std::string(message) + ": actual=\"" + actual.ToStdString() + "\" expected=\"" + expected.ToStdString() + "\"");
    }

    template<typename F>
    void RequireThrows(F&& fn, const char* message) {
        try {
            fn();
        } catch (const std::exception&) {
            return;
        }
        throw std::runtime_error(message);
    }

    // 验证 Result 将两个自包含数值上下文原样交给延迟工厂。
    LikesProgram::Status BuildLazyStatus(
        size_t primaryContext, size_t secondaryContext, const char16_t* staticMessage) {
        g_lazyStatusFactoryCalls.fetch_add(1, std::memory_order_relaxed);
        if (primaryContext != 7 || secondaryContext != 11) return LikesProgram::Status::Internal(u"lazy context mismatch");
        return LikesProgram::Status::InvalidArgument(staticMessage);
    }

    std::u8string U8Bytes(std::initializer_list<unsigned char> bytes) {
        std::u8string result; // 构造包含非法字节序列的 UTF-8 测试输入
        for (unsigned char b : bytes) result.push_back(static_cast<char8_t>(b));
        return result;
    }

    // 逐个触发所有会在构造期间拒绝输入的 String 路径。
    void ExerciseInvalidStringConstructors() {
        const std::u8string invalidUtf8 = U8Bytes({ 0xC0, 0xAF }); // overlong UTF-8 样本
        const char invalidNarrow[] = { static_cast<char>(0xC0), static_cast<char>(0xAF), '\0' }; // char UTF-8 样本
        const char8_t invalidChar8[] = { static_cast<char8_t>(0xC0), static_cast<char8_t>(0xAF), u8'\0' }; // char8_t UTF-8 样本
        const std::u32string invalidUtf32{ 0xD800 }; // surrogate UTF-32 样本
        const char32_t invalidChar32[] = { 0xD800, U'\0' }; // 指针构造用 surrogate 样本

        RequireThrows([&] { LikesProgram::String value(invalidNarrow); }, "String const char* constructor should reject invalid UTF-8");
        RequireThrows([&] { LikesProgram::String value(invalidChar8); }, "String const char8_t* constructor should reject invalid UTF-8");
        RequireThrows([&] { LikesProgram::String value(std::string(invalidNarrow, 2)); }, "String std::string constructor should reject invalid UTF-8");
        RequireThrows([&] { LikesProgram::String value(invalidUtf8); }, "String std::u8string constructor should reject invalid UTF-8");
        RequireThrows([&] { LikesProgram::String value{ std::u8string_view(invalidUtf8) }; }, "String std::u8string_view constructor should reject invalid UTF-8");
        RequireThrows([&] { LikesProgram::String value(invalidChar32); }, "String const char32_t* constructor should reject surrogate input");
        RequireThrows([&] { LikesProgram::String value(invalidUtf32); }, "String std::u32string constructor should reject surrogate input");
        RequireThrows([&] { LikesProgram::String value{ std::u32string_view(invalidUtf32) }; }, "String std::u32string_view constructor should reject surrogate input");
        RequireThrows([] { LikesProgram::String value(static_cast<char32_t>(0xD800)); }, "String char32_t constructor should reject surrogate input");
        RequireThrows([] { LikesProgram::String value(4, static_cast<char32_t>(0xD800)); }, "String repeated char32_t constructor should reject surrogate input");
    }

    // Debug CRT 检查构造函数抛出后不会遗留 PImpl 或内部缓冲。
    void TestStringConstructorExceptionLifetime() {
        ExerciseInvalidStringConstructors(); // 预热异常运行时和标准库内部缓存

#if defined(_MSC_VER) && defined(_DEBUG)
        _CrtMemState before{}; // 重复异常构造前的 CRT 堆快照
        _CrtMemState after{}; // 重复异常构造后的 CRT 堆快照
        _CrtMemState difference{}; // 两个快照之间仍存活的分配
        _CrtMemCheckpoint(&before);
        for (int i = 0; i < 128; ++i) ExerciseInvalidStringConstructors();
        _CrtMemCheckpoint(&after);
        Require(_CrtMemDifference(&difference, &before, &after) == 0, "String throwing constructors should not leak CRT heap allocations");
#else
        for (int i = 0; i < 128; ++i) ExerciseInvalidStringConstructors();
#endif
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
        LikesProgram::String moveAssignedSource(U"assign-me"); // 移动赋值源也必须可写复用
        LikesProgram::String moveAssignedTarget(U"old-target");
        moveAssignedTarget = std::move(moveAssignedSource);
        RequireEq(moveAssignedTarget, U"assign-me", "String move assignment target mismatch");
        Require(moveAssignedSource.Empty(), "move-assigned String source should be empty");
        moveAssignedSource.Clear();
        moveAssignedSource.Append(U"assign-reuse");
        RequireEq(moveAssignedSource, U"assign-reuse", "move-assigned String source should be reusable");

        LikesProgram::String text(U"ababa"); // 重叠查找样本
        Require(text.Find(U"aba") == 0, "String Find overlap failed");
        Require(text.LastFind(U"aba", LikesProgram::String::npos) == 2, "String LastFind overlap failed");
        Require(text.LastFind(U"aba", 1) == 0, "String LastFind start bound failed");
        Require(text.LastFind(U"missing") == LikesProgram::String::npos, "String LastFind missing failed");

        LikesProgram::String splitText(U"A\U0001F600B\U0001F600C"); // 非 BMP 分隔符样本
        Require(splitText.Size() == 5, "String Size should count non-BMP as one code point");
        Require(splitText.size() == 5, "String size should count non-BMP as one code point");
        Require(splitText.Length() == 7, "String Length should count non-BMP as two UTF-16 code units");
        Require(splitText.length() == 7, "String length should count non-BMP as two UTF-16 code units");
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
        const std::string embeddedUtf8("a\0b", 3); // 显式长度 UTF-8 view 必须保留中间 NUL
        LikesProgram::String embeddedUtf8Text{ std::string_view(embeddedUtf8) };
        Require(embeddedUtf8Text.Length() == 3, "String std::string_view should preserve embedded NUL bytes");
        Require(embeddedUtf8Text.data()[1] == u'\0' && embeddedUtf8Text.data()[2] == u'b', "String std::string_view embedded NUL content mismatch");
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
        LikesProgram::String utf8Text(std::u8string(u8"Likes")); // 通过公开 String 构造覆盖 UTF-8 转 UTF-16
        auto utf8 = utf8Text.ToU8String(); // 通过公开导出覆盖 UTF-16 转 UTF-8
        Require(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) == "Likes", "Unicode round trip failed");

        LikesProgram::String nonBmpText(U"\U0001F600"); // 非 BMP 字符通过公开 UTF-32 构造进入内部 UTF-16
        auto nonBmpUtf8 = nonBmpText.ToU8String();       // 非 BMP UTF-8 输出验证 surrogate pair
        auto nonBmpUtf32 = nonBmpText.ToU32String();     // 非 BMP UTF-32 输出验证 code point
        Require(nonBmpUtf32.size() == 1 && nonBmpUtf32[0] == U'\U0001F600', "Unicode non-BMP round trip failed");
        Require(!nonBmpUtf8.empty(), "Unicode non-BMP UTF-8 output should not be empty");

        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xC0, 0xAF })); }, "Unicode should reject overlong UTF-8");
        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xE0, 0x80, 0x80 })); }, "Unicode should reject overlong three-byte UTF-8");
        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xED, 0xA0, 0x80 })); }, "Unicode should reject UTF-8 encoded surrogate");
        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xF4, 0x90, 0x80, 0x80 })); }, "Unicode should reject UTF-8 code points above U+10FFFF");
        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xE2, 0x82 })); }, "Unicode should reject truncated UTF-8");
        RequireThrows([] { LikesProgram::String(U8Bytes({ 0xE2, 0x28, 0xA1 })); }, "Unicode should reject invalid UTF-8 continuation");
        RequireThrows([] { LikesProgram::String text(std::u16string{ 0xD800 }); (void)text.ToU8String(); }, "Unicode should reject dangling UTF-16 high surrogate");
        RequireThrows([] { LikesProgram::String text(std::u16string{ 0xDC00 }); (void)text.ToU32String(); }, "Unicode should reject dangling UTF-16 low surrogate");
        RequireThrows([] { LikesProgram::String(std::u32string{ 0xD800 }); }, "Unicode should reject UTF-32 surrogate code point");
        RequireThrows([] { LikesProgram::String(std::u32string{ 0x110000 }); }, "Unicode should reject UTF-32 code point above U+10FFFF");
    }

    void TestFormat() {
        LikesProgram::String text = LikesProgram::String::Format(u"{} {:04d} {:#x}", u"Core", 7, 255);
        RequireEq(text, U"Core 0007 0xff", "String::Format failed");

        RequireEq(LikesProgram::String::Format(U"Hello, {}!", U"World"), U"Hello, World!", "String::Format basic automatic index failed");
        RequireEq(LikesProgram::String::Format(U"{1} + {0} = {2}", 2, 3, 5), U"3 + 2 = 5", "String::Format explicit index failed");
        RequireEq(LikesProgram::String::Format(U"{2}{}{}{}", 1, 2, 3, 4, 5), U"3124", "String::Format mixed index failed");
        RequireEq(LikesProgram::String::Format(U"{1}{}{1}{}{}", U"A", U"B", U"C", U"D", U"E"), U"BABCD", "String::Format repeated mixed index failed");
        RequireEq(LikesProgram::String::Format(U"{:*>8}", 42), U"******42", "String::Format right fill failed");
        RequireEq(LikesProgram::String::Format(U"{:'--'<10}", U"abc"), U"abc-------", "String::Format multi-code-point fill should be clipped to width");
        RequireEq(LikesProgram::String::Format(U"{:'ab'>5}", U"x"), U"ababx", "String::Format multi-code-point left padding failed");
        RequireEq(LikesProgram::String::Format(U"{:#08X}", 255), U"0X0000FF", "String::Format alternate uppercase hex zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:#08x}", 255), U"0x0000ff", "String::Format alternate lowercase hex zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:08d}", -42), U"-0000042", "String::Format signed decimal zero padding failed");
        RequireEq(LikesProgram::String::Format(U"{:+d} {: d}", 42, 42), U"+42  42", "String::Format sign control failed");
        RequireEq(LikesProgram::String::Format(U"{:.3f}", 3.14159), U"3.142", "String::Format floating precision failed");
        RequireEq(LikesProgram::String::Format(U"{:.3s}", U"\u4F60\u597Dabc"), U"\u4F60\u597Da", "String::Format string precision failed");
        RequireEq(LikesProgram::String::Format(U"{:.2s}", U"A\U0001F600B"), U"A\U0001F600", "String::Format string precision should preserve surrogate pairs");
        RequireEq(LikesProgram::String::Format(U"{} {}", std::string_view("sv"), std::u16string_view(u"u16")), U"sv u16", "String::Format std string_view arguments failed");
        const char32_t* formatPtr = U"ptr";
        const char32_t* nullU32Ptr = nullptr;
        RequireEq(LikesProgram::String::Format(U"{} {} {}", U"lit", formatPtr, std::u32string_view(U"view")), U"lit ptr view", "String::Format UTF-32 literal/pointer/view arguments failed");
        RequireEq(LikesProgram::String::Format(U"{}", nullU32Ptr), U"", "String::Format null UTF-32 pointer should format as empty string");

        const std::u32string inlineBoundary(128, U'x'); // 恰好占满格式化内联输出缓冲
        RequireEq(LikesProgram::String::Format(U"{}", std::u32string_view(inlineBoundary)), LikesProgram::String(inlineBoundary), "String::Format inline output boundary failed");

        const std::u32string longValue(256, U'y'); // 超过内联容量，验证迁移后的文本完整性
        std::u32string longExpected = U"head:";
        longExpected += longValue;
        longExpected += U":tail";
        RequireEq(LikesProgram::String::Format(U"head:{}:tail", std::u32string_view(longValue)), LikesProgram::String(longExpected), "String::Format long output fallback failed");

        const LikesProgram::String wideInteger = LikesProgram::String::Format(U"{:256d}", 42);
        Require(wideInteger.Length() == 256 && wideInteger.At(254) == U'4' && wideInteger.At(255) == U'2', "String::Format wide integer padding fallback failed");
        RequireEq(LikesProgram::String::Format(U"{{}}"), U"{}", "String::Format brace escaping failed");
        RequireEq(LikesProgram::String::Format(U"{99}", U"x"), U"{?}", "String::Format out-of-range index fallback failed");
        RequireEq(LikesProgram::String::Format(U"{:unknown}", U"x"), U"{!type}", "String::Format unknown named formatter fallback failed");
        RequireEq(LikesProgram::String::Format(U"{"), U"{!}", "String::Format unmatched brace fallback failed");

        RequireEq(LikesProgram::String::Format(U"{:umissing_formatter}", 1), U"{!type}", "String::Format unknown user formatter should use stable fallback");
    }

    void TestTime() {
        auto point = LikesProgram::Time::NsToSystemClock(LikesProgram::Time::Nanoseconds(123456700));
        auto text = LikesProgram::Time::FormatTime(point, u"%Y");
        Require(text.Length() == 4, "FormatTime year length failed");

        RequireEq(LikesProgram::Time::FormatTime(point, U"%3f %6f %9f"), U"123 123456 123456700", "FormatTime fractional seconds failed");
        auto apText = LikesProgram::Time::FormatTime(point, U"AP ap").ToStdString();
        Require(apText == "AM am" || apText == "PM pm", "FormatTime AP/ap casing failed");

        auto beforeEpoch = LikesProgram::Time::NsToSystemClock(LikesProgram::Time::Nanoseconds(-750000000));
        RequireEq(LikesProgram::Time::FormatTime(beforeEpoch, U"%9f"), U"250000000", "FormatTime negative time point fractional seconds failed");
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
        Require(version.major == 1 && version.minor == 0 && version.patch == 0, "Version current tuple failed");
        Require(LikesProgram::Version::IsAtLeast(1, 0, 0), "Version IsAtLeast failed");
        Require(LikesProgram::Version::CurrentString() == std::string_view("1.0.0"), "Version string failed");

        LikesProgram::Status ok; // 默认状态表示成功
        Require(ok.IsOk(), "Status default should be ok");
        auto invalid = LikesProgram::Status::InvalidArgument(U"bad input");
        Require(!invalid.IsOk(), "Status invalid argument should fail");
        Require(invalid.Code() == LikesProgram::StatusCode::InvalidArgument, "Status code mismatch");
        Require(invalid.ToString().StartsWith(U"InvalidArgument"), "Status ToString should include code name");

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
        Require(!assigned.IsOk() && assigned.GetStatus().Code() == LikesProgram::StatusCode::NotFound, "Result assignment from status branch failed");

        g_lazyStatusFactoryCalls.store(0, std::memory_order_relaxed);
        auto lazyFailure = LikesProgram::Result<int>::LazyFailure(&BuildLazyStatus, 7, 11, u"deferred failure"); // 尚未物化的自包含失败
        Require(!lazyFailure.IsOk() && g_lazyStatusFactoryCalls.load(std::memory_order_relaxed) == 0, "Result IsOk should not materialize a lazy failure");
        auto propagatedFailure = lazyFailure.PropagateFailure<LikesProgram::String>(); // 跨类型保留延迟状态
        Require(!propagatedFailure.IsOk() && g_lazyStatusFactoryCalls.load(std::memory_order_relaxed) == 0, "Result PropagateFailure should not materialize a lazy failure");
        Require(propagatedFailure.GetStatus().Message() == u"deferred failure" && g_lazyStatusFactoryCalls.load(std::memory_order_relaxed) == 1, "Result GetStatus should materialize the lazy failure once");
        Require(propagatedFailure.GetStatus().Message() == u"deferred failure" && g_lazyStatusFactoryCalls.load(std::memory_order_relaxed) == 1, "Result repeated GetStatus should reuse the materialized failure");

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
        Require(std::to_integer<int>(bytes[1]) == 15 && std::to_integer<int>(bytes[2]) == 15, "ByteSpan Fill failed");
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
        Require(infinite.Remaining() == LikesProgram::Time::Duration::max(), "Infinite deadline remaining failed");

        Require(!LikesProgram::Platform::OperatingSystemName().empty(), "Platform OS name should not be empty");
        Require(!LikesProgram::Platform::ArchitectureName().empty(), "Platform architecture name should not be empty");
        Require(!LikesProgram::Platform::CompilerName().empty(), "Platform compiler name should not be empty");
        static_assert(LikesProgram::Platform::CxxStandard >= 202002L, "Platform C++ standard should be C++20 or newer");
    }
}

int main() {
    try {
        TestString();
        TestStringConstructorExceptionLifetime();
        TestUnicode();
        TestFormat();
        TestTime();
        TestTimer();
        TestCoreContracts();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
