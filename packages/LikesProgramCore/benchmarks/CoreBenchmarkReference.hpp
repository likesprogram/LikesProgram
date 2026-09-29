#pragma once
#include <cstddef>
#include <string>
#include <string_view>

#if defined(_WIN32) && defined(LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_SHARED)
#if defined(LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_EXPORTS)
#define LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API __declspec(dllexport)
#else
#define LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API __declspec(dllimport)
#endif
#else
#define LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API
#endif

// benchmark 私有的 std 字符串 PImpl，不进入安装或产品 ABI。
struct LikesProgramCoreBenchmarkStdString;

extern "C" {
    // 创建由参考库拥有的 std::u16string。
    LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API LikesProgramCoreBenchmarkStdString* LikesProgramCoreBenchmarkCreateStdString(const char16_t* text, std::size_t length);

    // 销毁参考库创建的 std::u16string。
    LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API void LikesProgramCoreBenchmarkDestroyStdString(LikesProgramCoreBenchmarkStdString* text) noexcept;

    // 跨目标构造 std::u16string_view，匹配 StringView 导入构造函数的写回 ABI。
    LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API void LikesProgramCoreBenchmarkConstructStdStringView(std::u16string_view* view, const LikesProgramCoreBenchmarkStdString* text) noexcept;

    // 跨参考目标读取 UTF-16 长度，匹配 String::Length() 的链接边界。
    LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API std::size_t LikesProgramCoreBenchmarkStdStringLength(const LikesProgramCoreBenchmarkStdString* text) noexcept;

    // 跨参考目标读取缓存后的 code point 数，匹配 String::Size() 的链接边界。
    LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API std::size_t LikesProgramCoreBenchmarkStdStringSize(const LikesProgramCoreBenchmarkStdString* text);

}

// 在参考目标内完成格式化，使用 C++ linkage 承载标准库返回类型。
LIKESPROGRAM_CORE_BENCHMARK_REFERENCE_API std::u16string LikesProgramCoreBenchmarkFormatStdString(int hexValue, int decValue, const wchar_t* text) noexcept;
