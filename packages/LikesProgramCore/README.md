# LikesProgramCore 使用手册

`LikesProgramCore` 是 LikesProgram 的基础包，也是所有扩展包共享的公共契约层。它不依赖第三方库，提供 Unicode 字符串、格式化、时间、计时器、状态返回、平台信息、字节视图等常用基础能力。

一句话：不知道先用哪个包时，先用 Core；Logging、Config 这些扩展包也都建立在 Core 上。

## 你需要知道的三件事

1. Core 总是构建，不需要额外打开选项。
2. 使用 Core 时链接 `LikesProgram::Core`。
3. `LikesProgram::String` 内部是 UTF-16，`Size()` 按 Unicode code point 计数，`Length()`/`size()`/`length()` 按 UTF-16 code unit 计数。

## 构建和链接

从仓库根目录构建：

```powershell
cd C:\Users\TX2\Desktop\LikesProgramProjects\LikesProgram
cmake -S . -B build-core -DLIKESPROGRAM_BUILD_LOGGING=OFF -DLIKESPROGRAM_BUILD_CONFIG=OFF
cmake --build build-core --config Debug
ctest --test-dir build-core -R LikesProgramCoreTests --output-on-failure -C Debug
```

在你的 CMake 项目中使用：

```cmake
find_package(LikesProgram CONFIG REQUIRED)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE LikesProgram::Core)
```

最小示例：

```cpp
#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <iostream>

int main() {
    LikesProgram::String name(u"LikesProgramCore");
    auto message = LikesProgram::String::Format(u"{} {}", name,
        LikesProgram::String(LikesProgram::Version::CurrentString()));

    std::cout << message.ToStdString() << std::endl;
    return 0;
}
```

## 公开头文件

推荐只包含这些安装到 `include/LikesProgram/Core` 下的公开头：

```cpp
#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/StringView.hpp>
#include <LikesProgram/Core/ByteSpan.hpp>
#include <LikesProgram/Core/Status.hpp>
#include <LikesProgram/Core/Result.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <LikesProgram/Core/Platform.hpp>
#include <LikesProgram/Core/ScopeGuard.hpp>
#include <LikesProgram/Core/Clock.hpp>
#include <LikesProgram/Core/Deadline.hpp>
#include <LikesProgram/Core/time/Time.hpp>
#include <LikesProgram/Core/time/Timer.hpp>
```

`src/include` 下的 `unicode/*`、`stringFormat/*` 服务于库自身构建和调试场景。二次开发时优先使用上面列出的公开头。

## 模块范围

Core 当前包含：

- `String`：Unicode 字符串，内部 UTF-16 存储。
- `String::Format`：与 `String` 集成的格式化能力。
- `Time` / `Clock` / `Deadline` / `Timer`：时间格式化、单位转换、单调时钟、超时截止时间、高精度计时。
- `Status` / `Result<T>` / `Result<void>`：异常之外的轻量返回契约。
- `StringView`：不拥有内存的 UTF-16 只读文本视图。
- `ByteSpan` / `ConstByteSpan`：不拥有内存的可写/只读字节视图。
- `ScopeGuard` / `NonCopyable`：作用域退出清理和禁止拷贝基类。
- `Platform`：操作系统、架构、编译器、C++ 标准、Core 构建形态。
- `Version`：统一版本信息。

Core 不包含 Logging、Config、Metrics、ThreadPool、Net、SSL/TLS、socket、HTTP/RPC、数据库、完整 JSON 解析器或业务框架。

## String 基础用法

`LikesProgram::String` 是拥有型字符串，内部保存 UTF-16，并保证 `data()` / `c_str()` 返回 NUL 结尾的只读 UTF-16 缓冲。

常见构造：

```cpp
#include <LikesProgram/Core/String.hpp>

using LikesProgram::String;

String empty;
String utf8("hello");                         // const char* 默认按 UTF-8
String utf8Explicit("hello", String::Encoding::UTF8);
String gbk(bytes, String::Encoding::GBK);      // bytes 是 const char*
String u8(u8"你好");
String u16(u"你好");
String u32(U"你好😀");
String wide(std::wstring(L"wide"));
String fromView(std::u16string_view(u"abc", 3));
String repeated(3, U'好');                    // "好好好"
String number(static_cast<int64_t>(42));       // "42"
String truth(true);                            // "true"
```

空指针字符串会被当成空串：

```cpp
const char* p = nullptr;
String s(p);           // 空串，不崩溃
```

严格 Unicode 行为：

- 非法 UTF-8、UTF-16、UTF-32 输入会抛出 `std::runtime_error`。
- UTF-32 surrogate code point 或超过 `U+10FFFF` 会抛出。
- Windows 上 UTF-8/UTF-16 转换使用严格 Win32 API。
- GBK 转换依赖平台实现，转换失败会抛异常。

读取长度：

```cpp
String text(U"A😀B");

auto codePoints = text.Size();    // 3，按 Unicode code point
auto codeUnits = text.Length();   // 4，UTF-16 中 😀 占两个 code unit

text.size();      // 等价 Length()
text.length();    // 等价 Length()
text.Empty();     // 是否为空
text.empty();     // 等价 Empty()
```

访问字符：

```cpp
char32_t first = text.Front();
char32_t last = text.Back();
char32_t second = text.At(1);
char32_t alsoSecond = text[1];
```

边界行为：

- `At(index)` 越界抛 `std::out_of_range`。
- `Front()` / `Back()` 对空串抛 `std::out_of_range`。
- 索引按 code point，不是 UTF-16 code unit。

追加和拼接：

```cpp
String text(u"Hello");
text.Append(u' ');
text.Append(U'😀');
text.Append(u" world");
text.Append(std::u16string_view(u"!", 1));

text += String(u" done");
String combined = String(u"A") + String(u"B");

text.append(String(u"x"));        // std 风格别名
text.clear();                     // 等价 Clear()
```

`Append(char32_t)` 对非法 code point 抛异常。自追加、重叠追加会做保护复制。

子串和查找：

```cpp
String s(U"A😀B😀C");

String mid = s.SubString(1, 3);      // "😀B😀"
String left = s.Left(2);             // "A😀"
String right = s.Right(2);           // "😀C"

auto pos = s.Find(U"B");             // 2
auto last = s.LastFind(U"😀");       // 3

bool starts = s.StartsWith(U"A");
bool ends = s.EndsWith(U"C");
```

边界行为：

- `SubString(index, count)` 超出范围时返回空串或截到末尾，不切断 surrogate pair。
- `Find(empty, start)` 返回 `start`，前提是 `start <= Size()`，否则返回 `npos`。
- `Find` / `LastFind` 失败返回 `String::npos`。
- 查找匹配会保证 code point 边界，不会从 surrogate pair 中间命中。

大小写和比较：

```cpp
String value(u"AbC");

String upper = value.ToUpper();
String lower = value.ToLower();

value.ToUpperInPlace();
value.ToLowerInPlace();

bool same = String(u"abc").EqualsIgnoreCase(u"ABC");
bool equal = String(u"a") == String(u"a");
bool ordered = String(u"a") < String(u"b");
```

大小写转换覆盖内置 BMP/SMP 映射表。它不是 locale 感知的自然语言排序或大小写规则。

编码转换：

```cpp
String text(U"你好😀");

std::string utf8 = text.ToStdString();                    // 默认 UTF-8
std::string gbk = text.ToStdString(String::Encoding::GBK);
std::u8string u8 = text.ToU8String();
std::wstring wide = text.ToWString();
std::u16string u16 = text.ToU16String();
std::u32string u32 = text.ToU32String();
```

隐式转换也存在：

```cpp
std::string s = String(u"abc");
std::u16string u16 = String(u"abc");
```

建议在公共代码里优先显式调用 `ToStdString()` / `ToU16String()`，可读性更好。

流输入输出：

```cpp
String text(u"hello");
std::cout << text;       // 按 text 内记录的 encoding 输出

String line;
std::cin >> line;        // 读取一整行，而不是只读一个单词
```

`operator>>` 使用 `std::getline`，会读取一行。

分割：

```cpp
auto parts = String(U"A😀B😀C").Split(U"😀");
// parts: "A", "B", "C"
```

空分隔符不会逐字符拆分，而是返回包含原字符串的单元素数组。

JSON 字符串转义：

```cpp
String escaped = String::EscapeJson(u"say \"hi\"\n");
// say \"hi\"\n
```

`EscapeJson` 只做字符串内容转义，不会给外层自动加双引号，也不是 JSON 解析器。

从 `std::any` 转成 String：

```cpp
std::any value = std::string_view("hello");
String out;

if (String::FromAny(value, out)) {
    // out == "hello"
}
```

支持常见整数、无符号整数、浮点、bool、标准字符串、字符串视图、字符指针、UTF-16/UTF-32 字符串和单字符类型。`std::any` 为空时返回 `false` 并输出空串。

并发规则：

- 多线程同时只读同一个 `String` 是支持的。
- 任意写操作，或读写并发，必须由调用方自行加锁。
- moved-from 的 `String` 保持空串可用，可继续赋值或追加。

## String::Format 使用手册

入口：

```cpp
String text = String::Format(u"{} + {} = {}", 1, 2, 3);
```

支持的格式串形态：

```text
{[index]:[fill][align][sign][#][0][width][.precision][type][typeExpand]}
```

普通例子：

```cpp
String::Format(u"Hello, {}!", u"World");          // Hello, World!
String::Format(u"{1} + {0} = {2}", 2, 3, 5);      // 3 + 2 = 5
String::Format(u"{:*>8}", 42);                    // ******42
String::Format(u"{:#08X}", 255);                  // 0X0000FF
String::Format(u"{:.3f}", 3.14159);               // 3.142
String::Format(u"{:.3s}", u"你好abc");            // 你好a
```

索引规则：

```cpp
String::Format(u"{} {}", u"A", u"B");             // 自动索引：A B
String::Format(u"{1} {0}", u"A", u"B");           // 显式索引：B A
String::Format(u"{2}{}{}{}", 1, 2, 3, 4, 5);      // 混合索引：3124
```

混合索引中，自动索引会选择还没被占用的最小参数索引。

大括号转义：

```cpp
String::Format(u"{{}}");       // {}
```

类型标识：

| 类型 | 含义 | 示例 |
| --- | --- | --- |
| `s` / `S` | 字符串 | `{:.3s}` |
| `d` / `i` | 十进制整数 | `{:04d}` |
| `x` / `X` | 十六进制整数 | `{:#08X}` |
| `b` / `B` | 二进制整数 | `{:#b}` |
| `o` / `O` | 八进制整数 | `{:#o}` |
| `f` / `F` | 定点浮点，默认精度 6 | `{:.2f}` |
| `e` / `E` | 科学计数法 | `{:.3E}` |
| `g` / `G` | 通用浮点 | `{:g}` |
| `%` | 百分比，数值乘 100 后加 `%` | `{:.1%}` |
| `c` | 字符 | `{:c}` |
| `p` / `P` | 指针地址 | `{:p}` |
| `t` / `T` | 时间格式化 | `{:tYYYY-MM-DD hh:mm:ss}` |
| `u` | 用户命名格式化器 | `{:uName}` |

对齐、宽度、填充：

```cpp
String::Format(u"{:<8}", u"x");          // x 后面补空格
String::Format(u"{:>8}", u"x");          // x 前面补空格
String::Format(u"{:^8}", u"x");          // 居中
String::Format(u"{:'--'<10}", u"abc");   // abc-------
String::Format(u"{:'ab'>5}", u"x");      // ababx
```

宽度和字符串精度按 Unicode code point 计数，不按 UTF-8 字节，也不按 UTF-16 code unit。

数值符号和前缀：

```cpp
String::Format(u"{:+d}", 42);     // +42
String::Format(u"{: d}", 42);     //  42
String::Format(u"{:#08x}", 255);  // 0x0000ff
```

错误回退：

```cpp
String::Format(u"{99}", u"x");        // {?}
String::Format(u"{:unknown}", u"x");  // {!type}
String::Format(u"{");                 // {!}
```

格式化 API 不应该因为参数不足、索引越界、未知类型或格式串错误导致进程崩溃。

时间格式：

```cpp
auto now = std::chrono::system_clock::now();

String a = String::Format(u"{:tYYYY/MM/DD hh:mm:ss}", now);
String b = String::Format(u"{:t%Y-%m-%d %H:%M:%S.%3f}", now);
```

时间格式实际调用 `LikesProgram::Time::FormatTime`，支持 `%Y`、`%m`、`%d`、`%H`、`%M`、`%S`、`%3f`、`%6f`、`%9f`、`YYYY`、`MM`、`DD`、`hh`、`mm`、`ss`、`SSS`、`AP`、`ap`、`TZ` 等。

库内扩展或调试场景下的自定义 formatter：

```cpp
#include <stringFormat/FormatInternal.hpp>

auto& internal = LikesProgram::StringFormat::FormatInternal::Instance();
internal.RegisterFormatter("upper", [](const LikesProgram::Any& value,
    const LikesProgram::StringFormat::FormatSpec&) {
    LikesProgram::String text;
    LikesProgram::String::FromAny(value, text);
    return text.ToUpper();
});

auto out = LikesProgram::String::Format(u"{:uupper}", u"abc"); // ABC
internal.UnregisterFormatter("upper");
```

普通业务代码优先使用 `String::Format` 内建能力。只有在需要调试格式化器注册表，或在库内扩展命名 formatter 时，才需要直接接触 `FormatInternal`。

## Time、Clock、Deadline、Timer

时间类型别名在 `LikesProgram::Time` 命名空间中：

```cpp
#include <LikesProgram/Core/time/Time.hpp>

using LikesProgram::Time::TimePoint;
using LikesProgram::Time::Duration;
using LikesProgram::Time::Nanoseconds;
using LikesProgram::Time::Milliseconds;
using LikesProgram::Time::Seconds;
```

格式化系统时间：

```cpp
auto now = std::chrono::system_clock::now();

auto text = LikesProgram::Time::FormatTime(now, u"%Y-%m-%d %H:%M:%S.%3f");
auto oldStyle = LikesProgram::Time::FormatTime(now, u"YYYY/MM/DD hh:mm:ss SSS");
```

单位转换：

```cpp
int64_t ns = LikesProgram::Time::MsToNs(1.5);      // 1500000
double ms = LikesProgram::Time::NsToMs(ns);        // 1.5
double seconds = LikesProgram::Time::MsToS(2500);  // 2.5

auto tp = LikesProgram::Time::NsToSystemClock(123456700);
auto dur = LikesProgram::Time::SystemClockToDuration(tp);
```

`ToLocalTime`：

```cpp
std::time_t t = std::time(nullptr);
std::tm local = LikesProgram::Time::ToLocalTime(t);
```

`Clock` 用于单调时间：

```cpp
#include <LikesProgram/Core/time/Clock.hpp>

auto begin = LikesProgram::Time::Clock::Now();
// do work
auto elapsed = LikesProgram::Time::Clock::Since(begin);
auto nowNs = LikesProgram::Time::Clock::NowNs();
auto systemNow = LikesProgram::Time::Clock::SystemNow();
```

`Deadline` 用于统一表达超时：

```cpp
#include <LikesProgram/Core/time/Deadline.hpp>

auto d1 = LikesProgram::Time::Deadline::FromNow(std::chrono::milliseconds(100));
auto d2 = LikesProgram::Time::Deadline::At(LikesProgram::Time::Clock::Now() + std::chrono::seconds(1));
auto never = LikesProgram::Time::Deadline::Infinite();

if (d1.Expired()) {
    // 超时
}

auto remaining = d1.Remaining();
bool hasLimit = d1.HasDeadline();
```

无限截止时间：

- `HasDeadline()` 返回 `false`。
- `Expired()` 返回 `false`。
- `Remaining()` 返回 `Duration::max()`。

`Timer` 用于高精度耗时统计：

```cpp
#include <LikesProgram/Core/time/Timer.hpp>

LikesProgram::Time::Timer timer(true); // 构造后自动 Start()
// do work
auto elapsed = timer.Stop();

auto ns = elapsed.count();
auto last = timer.GetLastElapsed();
auto total = timer.GetAccumulatedElapsed();

timer.Reset();
timer.Start();
```

父子计时器：

```cpp
LikesProgram::Time::Timer parent;
LikesProgram::Time::Timer child(false, &parent);

child.Start();
// child work
child.Stop();

auto childTotal = child.GetAccumulatedElapsed();
auto parentTotal = parent.GetAccumulatedElapsed(); // 包含 child 本次耗时
```

边界行为：

- `Stop()` 在未运行状态返回 `Duration(0)`。
- 连续 `Stop()` 第二次返回 0。
- `Reset()` 清空最近耗时、累计耗时并停止运行。
- 拷贝 `Timer` 不复制 running 状态，拷贝结果是非运行。
- 父计时器是非拥有指针，必须保证父对象活得比子计时器使用期更久。
- `NowNs()` 在 Windows 使用 `QueryPerformanceCounter`，其他平台使用单调时钟。

## Status 和 Result

`Status` 表达“不抛异常”的成功/失败状态。

```cpp
#include <LikesProgram/Core/Status.hpp>

LikesProgram::Status ok;
LikesProgram::Status alsoOk = LikesProgram::Status::OkStatus();
LikesProgram::Status invalid = LikesProgram::Status::InvalidArgument(u"bad input");
LikesProgram::Status missing = LikesProgram::Status::NotFound(u"missing file");
LikesProgram::Status internal = LikesProgram::Status::Internal(u"broken state");
LikesProgram::Status timeout = LikesProgram::Status::DeadlineExceeded(u"too slow");

if (!invalid) {
    auto code = invalid.Code();
    auto message = invalid.Message();
    auto text = invalid.ToString(); // InvalidArgument: bad input
}
```

状态码：

```text
Ok, Cancelled, InvalidArgument, NotFound, AlreadyExists,
PermissionDenied, ResourceExhausted, FailedPrecondition,
OutOfRange, Unimplemented, Internal, Unavailable,
DeadlineExceeded, Unknown
```

`StatusCodeName(code)` 返回稳定名称。

`Result<T>` 表达“要么有值，要么有失败状态”：

```cpp
#include <LikesProgram/Core/Result.hpp>

LikesProgram::Result<int> ParsePort(LikesProgram::String text) {
    if (text.Empty()) {
        return LikesProgram::Status::InvalidArgument(u"empty port");
    }
    return 8080;
}

auto result = ParsePort(u"8080");
if (result) {
    int value = result.Value();
} else {
    auto status = result.GetStatus();
}

int valueOrDefault = result.ValueOr(80);
```

边界行为：

- `Value()` 在失败结果上抛 `std::runtime_error`，消息来自 `Status::ToString()`。
- `MoveValue()` 移出成功值，失败时同样抛异常。
- 用 `Status::OkStatus()` 构造 `Result<T>` 这种“无值但成功”的情况会被转成内部错误，避免出现没有值的成功结果。

`Result<void>` 用于只关心成功/失败：

```cpp
LikesProgram::Result<void> Save() {
    return LikesProgram::Status::OkStatus();
}

auto saved = Save();
if (saved) {
    saved.Value(); // 成功时无操作
}
```

## StringView

`StringView` 是不拥有内存的 UTF-16 只读视图。

```cpp
#include <LikesProgram/Core/StringView.hpp>

LikesProgram::String owned(u"hello");
LikesProgram::StringView view(owned);

auto len = view.Length();          // UTF-16 code unit 数
bool empty = view.Empty();
const char16_t* data = view.Data();
auto stdView = view.ToStdView();
auto copy = view.ToString();
auto ch = view.At(0);
```

构造方式：

```cpp
LikesProgram::StringView empty;
LikesProgram::StringView fromString(owned);
LikesProgram::StringView fromPtr(u"abc");
LikesProgram::StringView fromPtrLen(u"abc\0x", 5);
LikesProgram::StringView fromStd(std::u16string_view(u"abc", 3));
```

边界行为：

- `StringView` 不延长底层字符串生命周期。
- 从 `String` 构造时，必须保证 `String` 活得比 `StringView` 更久。
- `At(index)` 越界抛 `std::out_of_range`。
- `operator[]` 不检查越界。
- `nullptr` 会构造为空视图。

## ByteSpan 和 ConstByteSpan

`ByteSpan` 是不拥有内存的可写字节视图；`ConstByteSpan` 是只读视图。

```cpp
#include <LikesProgram/Core/ByteSpan.hpp>

std::array<std::byte, 4> bytes{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}
};

LikesProgram::ByteSpan span(bytes.data(), bytes.size());

span.Fill(std::byte{0x0F});
span.At(0) = std::byte{0x01};

auto sub = span.SubSpan(1, 2);
auto readonly = span.AsConst();
auto hex = readonly.ToHexString(true); // 010F0F0F 这类大写十六进制
```

也可以从 `std::span` 构造：

```cpp
std::span<std::byte> writable(bytes);
LikesProgram::ByteSpan span(writable);

std::span<const std::byte> readonlyBytes(bytes);
LikesProgram::ConstByteSpan constSpan(readonlyBytes);
```

API 行为：

- `Data()` 返回 `std::byte*` 或 `const std::byte*`。
- `Bytes()` 返回 `uint8_t*` 或 `const uint8_t*`，方便对接 C API。
- `Size()` 返回字节数。
- `Empty()` 判断是否为空。
- `SubSpan(offset, count)` 越界抛 `std::out_of_range`。
- `count == npos` 表示截到末尾。
- `At(index)` 越界抛 `std::out_of_range`。
- `operator[]` 不检查越界。
- 构造时 data 为 `nullptr` 会得到空视图，即使传入 size 非 0。

## ScopeGuard 和 NonCopyable

`ScopeGuard` 在离开作用域时执行回调，适合临时恢复状态、释放 C API 资源、失败路径回滚。

```cpp
#include <LikesProgram/Core/ScopeGuard.hpp>

int value = 0;
{
    auto guard = LikesProgram::MakeScopeGuard([&] {
        value += 1;
    });
}
// value == 1
```

取消执行：

```cpp
auto guard = LikesProgram::MakeScopeGuard([&] {
    Rollback();
});

Commit();
guard.Dismiss(); // 成功提交后不回滚
```

边界行为：

- `ScopeGuard` 不可拷贝。
- 可移动，移动后源 guard 失效。
- 析构函数吞掉回调抛出的异常，避免析构路径再次抛出。

`NonCopyable` 是禁止拷贝的基类：

```cpp
class Handle : private LikesProgram::NonCopyable {
public:
    Handle() = default;
};
```

## Platform

`Platform` 提供编译期平台信息。

```cpp
#include <LikesProgram/Core/Platform.hpp>

auto os = LikesProgram::Platform::OperatingSystemName(); // Windows/Linux/MacOS/Unix/Unknown
auto arch = LikesProgram::Platform::ArchitectureName();  // x86/x64/arm/arm64/wasm/unknown
auto compiler = LikesProgram::Platform::CompilerName();  // MSVC/Clang/GCC/Unknown

bool isWindows = LikesProgram::Platform::IsWindows();
bool unixLike = LikesProgram::Platform::IsUnixLike();
bool coreShared = LikesProgram::Platform::CoreShared;
long standard = LikesProgram::Platform::CxxStandard;
```

枚举：

```cpp
LikesProgram::Platform::OperatingSystem::Windows;
LikesProgram::Platform::Architecture::X64;
LikesProgram::Platform::Compiler::MSVC;
```

`CoreShared` 由 `LIKESPROGRAM_CORE_SHARED` 编译定义决定。动态库构建时为 `true`，静态库构建时为 `false`。

## Version

版本信息在 `LikesProgram::Version` 命名空间：

```cpp
#include <LikesProgram/Core/Version.hpp>

static_assert(LikesProgram::Version::Major == 1);
static_assert(LikesProgram::Version::Minor == 0);
static_assert(LikesProgram::Version::Patch == 0);

auto name = LikesProgram::Version::Name;              // "LikesProgram"
auto version = LikesProgram::Version::VersionString;  // "1.0.0"
auto current = LikesProgram::Version::Current();
auto currentText = LikesProgram::Version::CurrentString();

if (LikesProgram::Version::IsAtLeast(1, 0, 0)) {
    // OK
}
```

`Version::Info` 字段：

```cpp
struct Info {
    int major;
    int minor;
    int patch;
    std::string_view suffix;
};
```

## 运行行为说明

- `String::Format` 支持自动/显式/混合索引、符号、进制前缀、零填充、多 code point 填充、字符串精度和错误回退；上面的格式化章节已经列出常用语法。
- `String` 对 UTF-8/UTF-16/UTF-32 输入采用严格转换策略，非法编码会抛出 `std::runtime_error`。
- moved-from 的 `String` 会保持为空串状态，可以继续赋值、追加或析构。
- 拷贝 `Timer` 时不会复制 running 状态，拷贝出来的新计时器是停止状态。
- 多线程同时只读同一个 `String` 可以正常使用；如果有线程会修改字符串，请在业务层加锁。
- `String::Length()` 返回 UTF-16 code unit 数，适合需要底层存储长度的场景。
- `String::Size()` 返回 Unicode code point 数，适合按字符语义截取、查找和遍历。
- `String::Format` 会缓存重复格式串的解析结果，常见内建类型也会走较轻量的格式化路径。

## 使用注意事项

- 需要字节长度时，请先转换到目标编码后再取 `std::string::size()`，例如 `text.ToStdString().size()`。
- 展示给用户的“可见字符数”可能涉及 grapheme cluster，例如 emoji 组合；当前 `Size()` 的单位是 Unicode code point。
- `StringView` 和 `ByteSpan` 都是不拥有内存的视图，适合短期传参；保存它们时要确保原始字符串或缓冲区仍然存在。
- `ScopeGuard` 的回调适合做清理动作；如果回调抛异常，析构函数会吞掉异常，避免离开作用域时再次抛出。
- `Timer` 的父计时器是普通指针关系，父对象需要覆盖子计时器使用期。

## 更多可运行样例

- `examples/CoreExample.cpp`：Core 常用组合示例。
- `tests/CoreTests.cpp`：完整行为测试，覆盖字符串、格式化、时间、状态、视图、字节视图和平台信息。
- `benchmarks/CoreBenchmark.cpp`：基础性能观察样例。
