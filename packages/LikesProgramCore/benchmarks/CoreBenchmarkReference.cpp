#include "CoreBenchmarkReference.hpp"

#include <atomic>
#include <array>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>

#if defined(__has_include)
#if __has_include(<format>)
#include <format>
#endif
#endif

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#define LP_BENCH_REFERENCE_HAS_STD_FORMAT 1
#else
#define LP_BENCH_REFERENCE_HAS_STD_FORMAT 0
#endif

#ifdef _MSC_VER
#define LP_BENCH_REFERENCE_NOINLINE __declspec(noinline)
#else
#define LP_BENCH_REFERENCE_NOINLINE __attribute__((noinline))
#endif

namespace {
    std::size_t CountCodePoints(std::u16string_view text) noexcept {
        std::size_t count = 0;
        for (std::size_t i = 0; i < text.size();) {
            const char16_t value = text[i];
            if (value >= 0xD800 && value <= 0xDBFF && i + 1 < text.size() &&
                text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                i += 2;
            } else ++i;
            ++count;
        }
        return count;
    }

    std::u16string WStringToUtf16(std::wstring_view text) {
#if WCHAR_MAX == 0xFFFF
        return std::u16string(reinterpret_cast<const char16_t*>(text.data()), text.size());
#else
        std::u16string result;
        result.reserve(text.size());
        for (wchar_t value : text) result.push_back(static_cast<char16_t>(value));
        return result;
#endif
    }
}

// 参考字符串把标准库状态放在独立目标内，模拟 Core 的 PImpl 所有权。
struct LikesProgramCoreBenchmarkStdString {
    struct Impl {
        explicit Impl(std::u16string_view text) : m_text(text), m_codePointCount(CountCodePoints(text)) { }

        std::u16string m_text; // 参考库拥有的 UTF-16 文本
        std::atomic<std::size_t> m_codePointCount; // 与 Core 缓存命中路径一致的原子计数
    };

    explicit LikesProgramCoreBenchmarkStdString(std::u16string_view text) : m_impl(std::make_unique<Impl>(text)) { }

    std::unique_ptr<Impl> m_impl; // 保留与 String owner -> PImpl 一致的间接层
};

namespace {
    // 缓存 miss 保留独立的潜在异常调用边界，匹配 String::UpdateCodePointCount() 控制流。
    LP_BENCH_REFERENCE_NOINLINE void UpdateCodePointCount(LikesProgramCoreBenchmarkStdString::Impl* impl) {
        impl->m_codePointCount.store(CountCodePoints(impl->m_text), std::memory_order_release);
    }
}

LikesProgramCoreBenchmarkStdString* LikesProgramCoreBenchmarkCreateStdString(const char16_t* text, std::size_t length) {
    const char16_t* source = text ? text : u""; // 空指针统一映射为空文本
    const std::size_t sourceLength = text ? length : 0; // 实际参与构造的 UTF-16 长度
    return new LikesProgramCoreBenchmarkStdString(std::u16string_view(source, sourceLength));
}

void LikesProgramCoreBenchmarkDestroyStdString(LikesProgramCoreBenchmarkStdString* text) noexcept {
    delete text;
}

void LikesProgramCoreBenchmarkConstructStdStringView(std::u16string_view* view, const LikesProgramCoreBenchmarkStdString* text) noexcept {
    if (!view) return;
    *view = text && text->m_impl ? std::u16string_view(text->m_impl->m_text) : std::u16string_view{};
}

std::size_t LikesProgramCoreBenchmarkStdStringLength(const LikesProgramCoreBenchmarkStdString* text) noexcept {
    return text && text->m_impl ? text->m_impl->m_text.size() : 0;
}

std::size_t LikesProgramCoreBenchmarkStdStringSize(const LikesProgramCoreBenchmarkStdString* text) {
    if (!text || !text->m_impl) return 0;
    const std::size_t cached = text->m_impl->m_codePointCount.load(std::memory_order_acquire);
    if (cached != std::numeric_limits<std::size_t>::max()) return cached;
    UpdateCodePointCount(text->m_impl.get());
    return text->m_impl->m_codePointCount.load(std::memory_order_acquire);
}

std::u16string LikesProgramCoreBenchmarkFormatStdString(int hexValue, int decValue, const wchar_t* text) noexcept {
    try {
#if LP_BENCH_REFERENCE_HAS_STD_FORMAT
        return WStringToUtf16(std::format(L"{:#08X}-{:08d}-{}", hexValue, decValue, text));
#else
        std::array<wchar_t, 128> buffer{};
        const int written = std::swprintf(buffer.data(), buffer.size(), L"0X%06X-%08d-%ls", hexValue, decValue, text);
        if (written < 0) return u"{!}";
        return WStringToUtf16(std::wstring_view(buffer.data(), static_cast<std::size_t>(written)));
#endif
    } catch (...) {
        return u"{!}";
    }
}
