#include <LikesProgram/Core/String.hpp>
#include <unicode/Unicode.hpp>
#include <stringFormat/FormatInternal.hpp>
#include <stdexcept>
#include <cwchar>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <array>
#include <iostream>
#include <sstream>
#include <locale>
#include <iomanip>
#include <cwctype>
#include <cctype>
#include <regex>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <iomanip>

namespace LikesProgram {
    struct String::StringImpl {
        mutable std::shared_mutex m_cpCacheMutex; // 保护 code point 数量与偏移缓存
        mutable std::atomic<size_t> m_cpCount = 0; // 缓存的 Unicode code point 数
        size_t m_capacity = 0;                    // 已分配的 UTF-16 code unit 容量
        std::unique_ptr<char16_t[]> m_data;  // UTF-16 数据
        size_t m_size;                        // UTF-16 单元长度
        Encoding m_encoding;                     // 原始编码
        mutable std::vector<size_t> m_cpOffsets; // 每个 Unicode code point 在 UTF-16 中的偏移
        mutable std::atomic<bool> m_cpCountValid = false;   // m_cpCount 是否可直接读取
        mutable std::atomic<bool> m_cpOffsetsValid = false; // m_cpOffsets 是否覆盖当前内容
    };

    namespace {
        constexpr size_t kMinStringCapacity = 8; // 小字符串追加时的最小预留容量

        // 追加扩容使用倍增策略，减少高频 Append 的重新分配次数。
        size_t GrowCapacity(size_t current, size_t required) {
            if (required == std::numeric_limits<size_t>::max()) {
                throw std::length_error("String capacity overflow");
            }

            size_t capacity = current < kMinStringCapacity ? kMinStringCapacity : current; // 本轮候选容量
            while (capacity < required) {
                if (capacity > std::numeric_limits<size_t>::max() / 2) {
                    return required;
                }
                capacity *= 2;
            }
            return capacity;
        }

        // 判断 UTF-16 高代理，供 code point 遍历复用。
        bool IsHighSurrogate(char16_t c) noexcept {
            return c >= 0xD800 && c <= 0xDBFF;
        }

        // 判断 UTF-16 低代理，供 code point 遍历复用。
        bool IsLowSurrogate(char16_t c) noexcept {
            return c >= 0xDC00 && c <= 0xDFFF;
        }

        // 判断 offset 是否落在 code point 边界，避免从 surrogate pair 中间切开。
        bool IsCodePointBoundary(const char16_t* data, size_t size, size_t offset) noexcept {
            if (offset == 0 || offset >= size) return true;
            return !(IsHighSurrogate(data[offset - 1]) && IsLowSurrogate(data[offset]));
        }

        // 预计算 UTF-32 输入转 UTF-16 后需要的 code unit 数，并做严格校验。
        size_t Utf16LengthForUtf32(std::u32string_view s) {
            size_t size = 0; // 输出 UTF-16 code unit 数
            for (char32_t cp : s) {
                if (cp >= 0xD800 && cp <= 0xDFFF) throw std::runtime_error("Invalid UTF-32 surrogate codepoint");
                if (cp <= 0xFFFF) {
                    ++size;
                }
                else if (cp <= 0x10FFFF) {
                    size += 2;
                }
                else {
                    throw std::runtime_error("Invalid UTF-32 codepoint");
                }
            }
            return size;
        }

        // 将已校验长度的 UTF-32 内容写入 UTF-16 缓冲区。
        void WriteUtf32AsUtf16(std::u32string_view s, char16_t* out) {
            size_t j = 0; // 当前写入的 UTF-16 code unit 偏移
            for (char32_t cp : s) {
                if (cp <= 0xFFFF) {
                    out[j++] = static_cast<char16_t>(cp);
                }
                else {
                    cp -= 0x10000;
                    out[j++] = static_cast<char16_t>((cp >> 10) + 0xD800);
                    out[j++] = static_cast<char16_t>((cp & 0x3FF) + 0xDC00);
                }
            }
        }

        // 在已构建的偏移表中查找 UTF-16 偏移对应的 code point 索引。
        size_t CodePointIndexFromOffset(const std::vector<size_t>& offsets, size_t offset) noexcept {
            auto it = std::lower_bound(offsets.begin(), offsets.end(), offset); // offset 表中的候选位置
            if (it == offsets.end() || *it != offset) return static_cast<size_t>(-1);
            return static_cast<size_t>(it - offsets.begin());
        }

        // 线性统计指定 UTF-16 偏移前的 code point 数，用于避免构建完整偏移表。
        size_t CountCodePointsBeforeOffset(const char16_t* data, size_t size, size_t offset) noexcept {
            size_t count = 0; // 已经过的 code point 数
            size_t i = 0;     // 当前 UTF-16 code unit 偏移
            while (i < offset && i < size) {
                if (IsHighSurrogate(data[i]) && i + 1 < size && IsLowSurrogate(data[i + 1])) {
                    i += 2;
                }
                else {
                    ++i;
                }
                ++count;
            }
            return count;
        }

        // 线性查找 code point 索引对应的 UTF-16 偏移，避免 Find 热路径构建完整缓存。
        size_t OffsetFromCodePointIndexLinear(const char16_t* data, size_t size, size_t index) noexcept {
            size_t offset = 0; // 当前 UTF-16 code unit 偏移
            size_t count = 0;  // 已经过的 code point 数
            while (offset < size && count < index) {
                if (IsHighSurrogate(data[offset]) && offset + 1 < size && IsLowSurrogate(data[offset + 1])) {
                    offset += 2;
                }
                else {
                    ++offset;
                }
                ++count;
            }
            return offset;
        }

        // 将一段 UTF-16 直接转换为 UTF-8，Windows 下使用严格系统 API。
        std::u8string Utf16RangeToUtf8(const char16_t* data, size_t size) {
            if (size == 0) return {};
#ifdef _WIN32
            if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("UTF-16 input too large");
            }
            const auto* wide = reinterpret_cast<const wchar_t*>(data); // Windows wchar_t 与 UTF-16 code unit 同宽
            const int inputSize = static_cast<int>(size);              // Windows API 接收的 UTF-16 code unit 数
            const int sizeNeeded = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, inputSize, nullptr, 0, nullptr, nullptr); // 目标 UTF-8 字节数
            if (sizeNeeded <= 0) throw std::runtime_error("Invalid UTF-16 string");

            std::u8string result(static_cast<size_t>(sizeNeeded), u8'\0'); // 按精确字节数预分配输出
            const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, inputSize, // 实际写入字节数
                reinterpret_cast<char*>(result.data()), sizeNeeded, nullptr, nullptr);
            if (written != sizeNeeded) throw std::runtime_error("Utf16ToUtf8 failed");
            return result;
#else
            return Unicode::Convert::Utf16ToUtf8(std::u16string(data, size));
#endif
        }
    }

    bool String::FromAny(const std::any& a, String& outContent) {
        if (!a.has_value()) {
            outContent = String();
            return false;
        }

        const std::type_info& type = a.type(); // std::any 内保存值的动态类型
        try {// 整数类型
            if (type == typeid(int)) { outContent = String(static_cast<int64_t>(std::any_cast<int>(a))); return true; }
            if (type == typeid(long)) { outContent = String(static_cast<int64_t>(std::any_cast<long>(a))); return true; }
            if (type == typeid(long long)) { outContent = String(static_cast<int64_t>(std::any_cast<long long>(a))); return true; }
            // 无符号整数统一提升到 uint64_t，避免不同平台 long 宽度影响输出。
            if (type == typeid(unsigned int)) { outContent = String(static_cast<uint64_t>(std::any_cast<unsigned int>(a))); return true; }
            if (type == typeid(unsigned long)) { outContent = String(static_cast<uint64_t>(std::any_cast<unsigned long>(a))); return true; }
            if (type == typeid(unsigned long long)) { outContent = String(static_cast<uint64_t>(std::any_cast<unsigned long long>(a))); return true; }

            // 浮点类型
            if (type == typeid(float)) { outContent = String(static_cast<long double>(std::any_cast<float>(a))); return true; }
            if (type == typeid(double)) { outContent = String(static_cast<long double>(std::any_cast<double>(a))); return true; }
            if (type == typeid(long double)) { outContent = String(std::any_cast<long double>(a)); return true; }

            // 布尔类型
            if (type == typeid(bool)) { outContent = String(std::any_cast<bool>(a)); return true; }

            // 字符串类型
            if (type == typeid(std::string)) { outContent = String(std::any_cast<std::string>(a)); return true; }
            if (type == typeid(std::string_view)) { outContent = String(std::any_cast<std::string_view>(a)); return true; }
            if (type == typeid(const char*)) { outContent = String(std::any_cast<const char*>(a)); return true; }
            // UTF-8 家族保留原始字节解释方式，再进入内部 UTF-16 存储。
            if (type == typeid(std::u8string)) { outContent = String(std::any_cast<std::u8string>(a)); return true; }
            if (type == typeid(std::u8string_view)) { outContent = String(std::any_cast<std::u8string_view>(a)); return true; }
            if (type == typeid(std::wstring)) { outContent = String(std::any_cast<std::wstring>(a)); return true; }
            // 宽字符指针按平台 wchar_t 宽度走构造函数兼容逻辑。
            if (type == typeid(std::wstring_view)) { outContent = String(std::any_cast<std::wstring_view>(a)); return true; }
            if (type == typeid(const wchar_t*)) { outContent = String(std::any_cast<const wchar_t*>(a)); return true; }
            if (type == typeid(std::u16string)) { outContent = String(std::any_cast<std::u16string>(a)); return true; }
            // UTF-16 视图和指针无需转码，但必须复制为内部拥有缓冲。
            if (type == typeid(std::u16string_view)) { outContent = String(std::any_cast<std::u16string_view>(a)); return true; }
            if (type == typeid(const char16_t*)) { outContent = String(std::any_cast<const char16_t*>(a)); return true; }
            // UTF-32 家族会做严格 code point 校验后写入 UTF-16。
            if (type == typeid(std::u32string)) { outContent = String(std::any_cast<std::u32string>(a)); return true; }
            if (type == typeid(std::u32string_view)) { outContent = String(std::any_cast<std::u32string_view>(a)); return true; }
            if (type == typeid(const char32_t*)) { outContent = String(std::any_cast<const char32_t*>(a)); return true; }

            // 单字符类型
            if (type == typeid(char)) { outContent = String(std::any_cast<char>(a)); return true; }
            if (type == typeid(char16_t)) { outContent = String(std::any_cast<char16_t>(a)); return true; }
            if (type == typeid(char32_t)) { outContent = String(std::any_cast<char32_t>(a)); return true; }
        }
        catch (...) {
            return false;
        }
        return true;
    }

    String::String(): m_impl(new StringImpl{}) {
        // 空串也分配 NUL 缓冲，保证 data()/c_str() 始终可用。
        m_impl->m_data = (std::make_unique<char16_t[]>(1));
        m_impl->m_size = 0;
        m_impl->m_encoding = Encoding::UTF8;
    }

    String::String(const char* s, Encoding enc): m_impl(new StringImpl{}) {
        m_impl->m_encoding = enc;
        if (!s) s = "";

        switch (enc) {
        case Encoding::UTF8: {
            size_t len = std::strlen(s); // 输入 UTF-8 字节串长度
            auto utf16 = Unicode::Convert::Utf8ToUtf16( // 构造用 UTF-16 中间结果
                std::u8string(reinterpret_cast<const char8_t*>(s),
                    reinterpret_cast<const char8_t*>(s) + len)
            );
            // 内部统一使用 UTF-16，并额外保留一位 NUL 终止符。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
            // 所有构造路径都写入 NUL，保证 c_str() 兼容 C 风格调用。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::GBK: {
            auto utf16 = Unicode::Convert::GbkToUtf16(std::string(s)); // 构造用 UTF-16 中间结果
            // GBK 也立即归一到 UTF-16，避免后续操作反复转码。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
            // GBK 转换后也保持 NUL 终止，便于后续无分支访问。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::UTF16: {
            const char16_t* ps = reinterpret_cast<const char16_t*>(s);
            size_t len = 0; // 输入 UTF-16 NUL 结尾序列长度
            while (ps[len] != 0) ++len;
            // UTF-16 输入可以直接拷贝，但仍保持独立拥有的缓冲。
            m_impl->m_size = len;
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), ps, m_impl->m_size * sizeof(char16_t));
            // 原始 UTF-16 输入复制后补终止符，不依赖源缓冲。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::UTF32: {
            const char32_t* ps = reinterpret_cast<const char32_t*>(s);
            size_t len = 0; // 输入 UTF-32 NUL 结尾序列长度
            while (ps[len] != 0) ++len;
            std::u32string_view view(ps, len);
            // UTF-32 先预估 UTF-16 长度，避免写入时二次扩容。
            m_impl->m_size = Utf16LengthForUtf32(view);
            m_impl->m_capacity = m_impl->m_size;
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            // 转写函数只负责内容，终止符由构造函数统一维护。
            WriteUtf32AsUtf16(view, m_impl->m_data.get());
            // UTF-32 转写完成后补 NUL，保持内部不变量。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for const char*");
        }
    }

    String::String(const char8_t* s): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF8;
        if (!s) s = u8"";
        auto utf16 = Unicode::Convert::Utf8ToUtf16(std::u8string(s)); // 构造用 UTF-16 中间结果
        // char8_t 指针按 UTF-8 解码后复制到内部缓冲。
        m_impl->m_size = utf16.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
        // 指针构造不能借用外部内存，写入私有 NUL 缓冲。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const char16_t* s): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
        if (!s) s = u"";
        size_t len = 0; // 输入 UTF-16 NUL 结尾序列长度
        while (s[len] != 0) ++len;
        // NUL 结尾 UTF-16 输入复制为独立可变存储。
        m_impl->m_size = len;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), s, m_impl->m_size * sizeof(char16_t));
        // 保持 NUL 终止，避免 c_str() 访问越界。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const char16_t* s, size_t length): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
        if (!s) {
            s = u"";
            length = 0;
        }
        // 显式长度构造允许输入包含 NUL，长度完全由调用方提供。
        m_impl->m_size = length;
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        // 长度为 0 时 memcpy 长度为 0，仍保持空串缓冲有效。
        std::memcpy(m_impl->m_data.get(), s, m_impl->m_size * sizeof(char16_t));
        // 显式长度可能包含内嵌 NUL，尾部仍追加库自己的终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const char32_t* s): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF32;
        if (!s) s = U"";
        size_t len = 0; // 输入 UTF-32 NUL 结尾序列长度
        while (s[len] != 0) ++len;
        std::u32string_view view(s, len);
        // UTF-32 指针路径保持严格 Unicode 范围校验。
        m_impl->m_size = Utf16LengthForUtf32(view);
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        // 转写函数只负责内容，终止符由构造函数统一维护。
        WriteUtf32AsUtf16(view, m_impl->m_data.get());
        // 写入完成后补终止符，保持 data()/c_str() 契约。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const String& other): m_impl(new StringImpl{}) {

        std::shared_lock otherLock(other.m_impl->m_cpCacheMutex);
        // 拷贝时连同 NUL 终止符一起复制，保证目标 data() 可直接使用。
        if (other.m_impl->m_data) {
            m_impl->m_data = std::make_unique<char16_t[]>(other.m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), other.m_impl->m_data.get(), (other.m_impl->m_size + 1) * sizeof(char16_t));
        } else {
            m_impl->m_data = std::make_unique<char16_t[]>(1);
            m_impl->m_data[0] = u'\0';
        }

        // code point 缓存状态在锁内复制，避免读到一半更新的数据。
        m_impl->m_size = other.m_impl->m_size;
        m_impl->m_capacity = other.m_impl->m_size;
        m_impl->m_encoding = other.m_impl->m_encoding;
        // 偏移表可以复制，因为内容缓冲已完整复制。
        m_impl->m_cpOffsets = other.m_impl->m_cpOffsets;
        m_impl->m_cpCountValid.store(other.m_impl->m_cpCountValid.load(std::memory_order_acquire), std::memory_order_release);
        m_impl->m_cpOffsetsValid.store(other.m_impl->m_cpOffsetsValid.load(std::memory_order_acquire), std::memory_order_release);
        // code point 数是原子缓存值，复制时不需要额外计算。
        m_impl->m_cpCount.store(other.m_impl->m_cpCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    String::String(String&& other) noexcept : m_impl(other.m_impl) {
        // moved-from 对象重置为空串，行为对齐 std 容器的可析构可赋值状态。
        other.m_impl = new StringImpl{};
        other.m_impl->m_data = std::make_unique<char16_t[]>(1);
        other.m_impl->m_data[0] = u'\0';
        // moved-from 对象只保留空内容与可用缓冲。
        other.m_impl->m_size = 0;
        other.m_impl->m_capacity = 0;
        // moved-from 对象统一回到 UTF-8 空串状态。
        // 空串的 code point 缓存直接标记有效，避免 moved-from 后 Size() 扫描。
        other.m_impl->m_encoding = Encoding::UTF8;
        other.m_impl->m_cpCountValid.store(true, std::memory_order_release);
        other.m_impl->m_cpOffsetsValid.store(true, std::memory_order_release);
    }

    String::String(char c, Encoding enc) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = enc;

        switch (enc) {
        case Encoding::UTF8: {
            char s[2] = { c, '\0' }; // 单字节输入的 NUL 结尾缓冲
            auto utf16 = Unicode::Convert::Utf8ToUtf16( // 单字符构造用 UTF-16 中间结果
                std::u8string(reinterpret_cast<const char8_t*>(s))
            );
            // 单字节 UTF-8 输入仍走统一解码，非法字节由转换层拒绝。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(),
                m_impl->m_size * sizeof(char16_t));
            // 单字符转换后也保持内部 NUL 终止。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::GBK: {
            std::string s(1, c);
            auto utf16 = Unicode::Convert::GbkToUtf16(s); // 单字符构造用 UTF-16 中间结果
            // GBK 单字节字符同样归一到 UTF-16 存储。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(),
                m_impl->m_size * sizeof(char16_t));
            // GBK 单字符可能产生一个或多个 UTF-16 单元，尾部统一补 NUL。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::UTF16: {
            // char 输入按无符号字节提升，避免负 char 符号扩展。
            m_impl->m_size = 1;
            m_impl->m_data = std::make_unique<char16_t[]>(2);
            m_impl->m_data[0] = static_cast<char16_t>(static_cast<unsigned char>(c));
            // 单单元路径手动写入 NUL 终止符。
            m_impl->m_data[1] = u'\0';
            break;
        }
        case Encoding::UTF32: {
            char32_t cp = static_cast<unsigned char>(c); // char 提升后的 Unicode code point
            auto utf16 = Unicode::Convert::Utf32ToUtf16(std::u32string(1, cp)); // 单字符构造用 UTF-16 中间结果
            // UTF-32 单字符也复用转换层，保持异常语义一致。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(),
                m_impl->m_size * sizeof(char16_t));
            // UTF-32 单字符转换后写入 NUL 终止符。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for char");
        }
    }

    String::String(const char8_t c): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF8;
        char8_t buf[2] = { c, 0 }; // 单 char8_t 输入的 NUL 结尾缓冲
        auto utf16 = Unicode::Convert::Utf8ToUtf16(std::u8string(buf)); // 单字符构造用 UTF-16 中间结果
        // char8_t 构造路径保持 UTF-8 校验后再落入 UTF-16。
        m_impl->m_size = utf16.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
        // 单 char8_t 构造结果使用私有 NUL 终止缓冲。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const char16_t c): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
        // 单 UTF-16 code unit 直接存储，调用方负责不传孤立代理。
        m_impl->m_size = 1;
        m_impl->m_data = std::make_unique<char16_t[]>(2);
        m_impl->m_data[0] = c;
        // 单 UTF-16 单元后追加终止符。
        m_impl->m_data[1] = u'\0';
    }

    String::String(const size_t count, const char16_t c): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;

        // 重复 BMP code unit 的构造不需要 code point 扫描。
        m_impl->m_size = count;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);

        for (size_t i = 0; i < count; ++i) { // 填充重复字符的 UTF-16 下标
            m_impl->m_data[i] = c;
        }

        m_impl->m_data[count] = u'\0';
    }

    String::String(const size_t count, const char32_t c) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF32;

        const size_t unitsPerChar = Utf16LengthForUtf32(std::u32string_view(&c, 1)); // 单个码点占用的 UTF-16 单元数
        if (count != 0 && unitsPerChar > std::numeric_limits<size_t>::max() / count) {
            throw std::length_error("String length overflow");
        }

        // 总长度按单个码点占用的 UTF-16 单元数线性计算。
        m_impl->m_size = count * unitsPerChar;
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        if (unitsPerChar == 1) {
            // BMP 码点可以直接批量填充为一个 UTF-16 单元。
            std::fill_n(m_impl->m_data.get(), count, static_cast<char16_t>(c));
        }
        else {
            char32_t cp = c - 0x10000; // 转为代理对计算用的补偿码点
            const char16_t pair[2] = { // 当前 SMP 码点对应的 UTF-16 代理对
                static_cast<char16_t>((cp >> 10) + 0xD800),
                static_cast<char16_t>((cp & 0x3FF) + 0xDC00)
            };
            for (size_t i = 0; i < count; ++i) { // 写入第 i 个重复码点
                m_impl->m_data[i * 2] = pair[0];
                m_impl->m_data[i * 2 + 1] = pair[1];
            }
        }
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const char32_t c): m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF32;
        // 单 UTF-32 码点先校验并计算最终 UTF-16 长度。
        m_impl->m_size = Utf16LengthForUtf32(std::u32string_view(&c, 1));
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        // 转写函数只负责内容，终止符由构造函数统一维护。
        WriteUtf32AsUtf16(std::u32string_view(&c, 1), m_impl->m_data.get());
        // 写入单码点后追加终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const std::string& s, Encoding enc) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = enc;
        switch (enc) {
        case Encoding::UTF8: {
            auto utf16 = Unicode::Convert::Utf8ToUtf16( // 构造用 UTF-16 中间结果
                std::u8string(reinterpret_cast<const char8_t*>(s.data()),
                    reinterpret_cast<const char8_t*>(s.data() + s.size()))
            );
            // std::string 默认按 UTF-8 解释，符合现代 std 互操作预期。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
            // std::string 构造结果持有独立 UTF-16 缓冲。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        case Encoding::GBK: {
            auto utf16 = Unicode::Convert::GbkToUtf16(s); // 构造用 UTF-16 中间结果
            // 指定 GBK 时先转码再进入统一 UTF-16 表示。
            m_impl->m_size = utf16.size();
            m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
            std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
            // GBK std::string 转换结果持有独立 UTF-16 缓冲。
            m_impl->m_data[m_impl->m_size] = u'\0';
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for std::string");
        }
    }

    String::String(std::string_view s, Encoding enc)
        : String(std::string(s), enc) {
    }

    String::String(const std::u8string& s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF8;
        auto utf16 = Unicode::Convert::Utf8ToUtf16(s); // 构造用 UTF-16 中间结果
        // u8string 已明确 UTF-8 编码，直接转换为内部 UTF-16。
        m_impl->m_size = utf16.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
        // u8string 转换结果补 NUL 终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(std::u8string_view s)
        : String(std::u8string(s)) {
    }

    String::String(const std::wstring& s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
#if WCHAR_MAX == 0xFFFF  // Windows
        // Windows wchar_t 与 UTF-16 同宽，可以直接复制单元。
        m_impl->m_size = s.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), s.data(), m_impl->m_size * sizeof(char16_t));
        // Windows 宽字符串复制后补库自己的终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
#else  // Linux
        std::u32string tmp; // Linux wchar_t 转 UTF-32 的中间缓冲
        for (wchar_t c : s) tmp.push_back(static_cast<char32_t>(c));
        auto utf16 = Unicode::Convert::Utf32ToUtf16(tmp); // 构造用 UTF-16 中间结果
        // POSIX wchar_t 常为 UTF-32，转换后再进入统一存储。
        m_impl->m_size = utf16.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
        // POSIX 宽字符串转换后补终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
#endif
    }

    String::String(std::wstring_view s)
        : String(std::wstring(s)) {
    }

    String::String(const std::u16string& s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
        // u16string 已是内部编码，复制后补 NUL 终止符。
        m_impl->m_size = s.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), s.c_str(), m_impl->m_size * sizeof(char16_t));
        // u16string 复制后补终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(std::u16string_view s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF16;
        // view 构造必须立即复制，避免引用外部短生命周期内存。
        m_impl->m_size = s.size();
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        // view 内容可能不以 NUL 结尾，因此只能按长度复制。
        std::memcpy(m_impl->m_data.get(), s.data(), m_impl->m_size * sizeof(char16_t));
        // u16string_view 复制后补终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(const std::u32string& s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF32;
        auto utf16 = Unicode::Convert::Utf32ToUtf16(s); // 构造用 UTF-16 中间结果
        // u32string 通过转换层校验 Unicode 范围。
        m_impl->m_size = utf16.size();
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        std::memcpy(m_impl->m_data.get(), utf16.c_str(), m_impl->m_size * sizeof(char16_t));
        // u32string 转换后补终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(std::u32string_view s) : m_impl(new StringImpl{}) {
        m_impl->m_encoding = Encoding::UTF32;
        // UTF-32 view 先计算目标长度，再一次性写入 UTF-16 缓冲。
        m_impl->m_size = Utf16LengthForUtf32(s);
        m_impl->m_capacity = m_impl->m_size;
        m_impl->m_data = std::make_unique<char16_t[]>(m_impl->m_size + 1);
        // 转写函数只负责内容，终止符由构造函数统一维护。
        WriteUtf32AsUtf16(s, m_impl->m_data.get());
        // u32string_view 转写后补终止符。
        m_impl->m_data[m_impl->m_size] = u'\0';
    }

    String::String(int64_t value) : String([&] {
        std::wostringstream woss; // 数值到宽字符串的格式化缓冲
        woss << value;
        return woss.str();
        }()) {
    }
    String::String(uint64_t value) : String([&] {
        std::wostringstream woss; // 数值到宽字符串的格式化缓冲
        woss << value;
        return woss.str();
        }()) {
    }
    String::String(long double value) : String([&] {
        std::wostringstream woss; // 数值到宽字符串的格式化缓冲
        woss << value;
        return woss.str();
        }()) {
    }
    String::String(bool value) : String(value ? u"true" : u"false") {
    }

    String::~String() {
        if (m_impl) delete m_impl;
        m_impl = nullptr;
    }

    String& String::operator=(const String& other) {
        if (this != &other) {
            // copy-and-swap 保持异常安全，失败时当前对象不变。
            String tmp(other);
            std::swap(m_impl, tmp.m_impl);
        }
        return *this;
    }

    String& String::operator=(String&& other) noexcept {
        if (this != &other) {
            // 释放当前实现后直接接管源对象 PImpl。
            if (m_impl) delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = new StringImpl{};
            // 源对象重建为空串，后续析构、赋值、Size() 都保持可靠。
            other.m_impl->m_data = std::make_unique<char16_t[]>(1);
            other.m_impl->m_data[0] = u'\0';
            other.m_impl->m_size = 0;
            // 源对象容量清零，避免误以为还拥有旧缓冲。
            other.m_impl->m_capacity = 0;
            other.m_impl->m_encoding = Encoding::UTF8;
            // 移动赋值后的源对象缓存也直接标记为空串有效。
            other.m_impl->m_cpCountValid.store(true, std::memory_order_release);
            other.m_impl->m_cpOffsetsValid.store(true, std::memory_order_release);
        }
        return *this;
    }

    String& String::operator=(const char* s) {
        return *this = String(s);
    }

    String& String::operator=(const char8_t* s) {
        return *this = String(s);
    }

    String& String::operator=(const char16_t* s) {
        return *this = String(s);
    }

    String& String::operator=(const char32_t* s) {
        return *this = String(s);
    }

    String& String::operator=(const std::string& s) {
        return *this = String(s);
    }

    String& String::operator=(std::string_view s) {
        return *this = String(s);
    }

    String& String::operator=(const std::u8string& s) {
        return *this = String(s);
    }

    String& String::operator=(std::u8string_view s) {
        return *this = String(s);
    }

    String& String::operator=(const std::wstring& s) {
        return *this = String(s);
    }

    String& String::operator=(std::wstring_view s) {
        return *this = String(s);
    }

    String& String::operator=(const std::u16string& s) {
        return *this = String(s);
    }

    String& String::operator=(std::u16string_view s) {
        return *this = String(s);
    }

    String& String::operator=(const std::u32string& s) {
        return *this = String(s);
    }

    String& String::operator=(std::u32string_view s) {
        return *this = String(s);
    }

    size_t String::Size() const {
        {
            if (m_impl->m_cpCountValid.load(std::memory_order_acquire)) return m_impl->m_cpCount.load(std::memory_order_relaxed);
        }

        UpdateCodePointCount();
        return m_impl->m_cpCount.load(std::memory_order_relaxed);
    }

    size_t String::Length() const {
        return m_impl->m_size;
    }

    bool String::Empty() const {
        return m_impl->m_size == 0;
    }

    const char16_t* String::data() const noexcept {
        return m_impl && m_impl->m_data ? m_impl->m_data.get() : u"";
    }

    const char16_t* String::c_str() const noexcept {
        return data();
    }

    void String::Clear() {
        if (m_impl->m_capacity < m_impl->m_size) m_impl->m_capacity = m_impl->m_size;
        if (!m_impl->m_data) {
            m_impl->m_data = std::make_unique<char16_t[]>(1);
            m_impl->m_capacity = 0;
        }
        // 清空内容但保留已有容量，匹配 std::basic_string 的复用预期。
        m_impl->m_data[0] = u'\0';
        m_impl->m_size = 0;
        std::unique_lock lock(m_impl->m_cpCacheMutex); // 清空后同步重置 code point 缓存
        m_impl->m_cpOffsets.clear();
        m_impl->m_cpCount.store(0, std::memory_order_relaxed);
        m_impl->m_cpCountValid.store(true, std::memory_order_release);
        // 空串没有偏移项，因此偏移缓存同样有效。
        m_impl->m_cpOffsetsValid.store(true, std::memory_order_release);
    }

    char32_t String::At(size_t index) const {
        UpdateCodePointCache();
        if (index >= m_impl->m_cpOffsets.size()) throw std::out_of_range("index out of range");
        size_t i = m_impl->m_cpOffsets[index]; // code point 对应的 UTF-16 起始偏移
        char16_t c = m_impl->m_data[i];       // 当前 code point 的首个 UTF-16 单元
        if (IsHighSurrogate(c) && i + 1 < m_impl->m_size && IsLowSurrogate(m_impl->m_data[i + 1])) {
            char16_t low = m_impl->m_data[i + 1]; // 当前代理对的低代理单元
            return 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        }
        return c;
    }

    char32_t String::operator[](size_t index) const {
        return At(index);
    }

    char32_t String::Front() const {
        if (Empty()) throw std::out_of_range("String::Front on empty string");
        return At(0);
    }

    char32_t String::Back() const {
        if (Empty()) throw std::out_of_range("String::Back on empty string");
        return At(Size() - 1);
    }

    String& String::Append(const String& str) {
        if (str.Empty()) return *this;
        if (this == &str) {
            String copy(str);
            return Append(copy);
        }
        return Append(std::u16string_view(str.m_impl->m_data.get(), str.m_impl->m_size));
    }

    String& String::Append(char16_t c) {
        return Append(std::u16string_view(&c, 1));
    }

    String& String::Append(char32_t c) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            throw std::runtime_error("Invalid UTF-32 surrogate codepoint");
        }
        if (c <= 0xFFFF) {
            return Append(static_cast<char16_t>(c));
        }
        if (c > 0x10FFFF) {
            throw std::runtime_error("Invalid UTF-32 codepoint");
        }

        c -= 0x10000;
        const char16_t pair[2] = { // 当前 SMP 码点对应的 UTF-16 代理对
            static_cast<char16_t>((c >> 10) + 0xD800),
            static_cast<char16_t>((c & 0x3FF) + 0xDC00)
        };
        return Append(std::u16string_view(pair, 2));
    }

    String& String::Append(std::u16string_view str) {
        if (str.empty()) return *this;
        const char16_t* source = str.data(); // 追加源缓冲区，可能指向当前 String 内部
        std::u16string ownedSource;          // 自追加或重叠追加时的保护副本
        if (m_impl->m_data) {
            const char16_t* begin = m_impl->m_data.get(); // 当前缓冲区起点
            const char16_t* end = begin + m_impl->m_size; // 当前有效 UTF-16 尾后位置
            if (source >= begin && source < end) {
                ownedSource.assign(str.begin(), str.end());
                source = ownedSource.data();
            }
        }
        const size_t oldSize = m_impl->m_size; // 追加前 UTF-16 code unit 数
        const size_t appendSize = str.size();  // 本次追加 UTF-16 code unit 数
        if (appendSize > std::numeric_limits<size_t>::max() - oldSize) {
            throw std::length_error("String append overflow");
        }

        const size_t newSize = oldSize + appendSize; // 追加后 UTF-16 code unit 数
        if (newSize == std::numeric_limits<size_t>::max()) {
            throw std::length_error("String append overflow");
        }
        if (m_impl->m_capacity < oldSize) m_impl->m_capacity = oldSize;

        if (newSize > m_impl->m_capacity) {
            const size_t newCapacity = GrowCapacity(m_impl->m_capacity, newSize); // 扩容后的 UTF-16 容量
            auto newData = std::make_unique<char16_t[]>(newCapacity + 1);         // 新缓冲区，额外 1 位放 NUL
            if (oldSize > 0) std::memcpy(newData.get(), m_impl->m_data.get(), oldSize * sizeof(char16_t));
            m_impl->m_data = std::move(newData);
            m_impl->m_capacity = newCapacity;
        }

        std::memmove(m_impl->m_data.get() + oldSize, source, appendSize * sizeof(char16_t));
        m_impl->m_size = newSize;
        m_impl->m_data[newSize] = u'\0';
        // 内容变化后必须失效 Size()/CodePointOffset 相关缓存。
        InvalidateCodePointCache();
        return *this;
    }

    String& String::Append(std::wstring_view str) {
#if WCHAR_MAX == 0xFFFF
        return Append(std::u16string_view(reinterpret_cast<const char16_t*>(str.data()), str.size()));
#else
        return Append(String(str));
#endif
    }

    String& String::operator+=(const String& str) {
        return Append(str);
    }

    String operator+(const String& lhs, const String& rhs) {
        String result(lhs); // 以左值副本作为拼接目标
        result.Append(rhs);
        return result;
    }

    std::ostream& operator<<(std::ostream& os, const String& str) {
        switch (str.m_impl->m_encoding) {
        case String::Encoding::GBK: {
            auto gbk = Unicode::Convert::Utf16ToGbk(std::u16string(str.m_impl->m_data.get(), str.m_impl->m_size)); // 输出用 GBK 字节串
            os << gbk;
            break;
        }
        case String::Encoding::UTF8: {
            auto utf8 = Unicode::Convert::Utf16ToUtf8(std::u16string(str.m_impl->m_data.get(), str.m_impl->m_size)); // 输出用 UTF-8 字节串
            os.write(reinterpret_cast<const char*>(utf8.data()), utf8.size() * sizeof(char8_t));
            break;
        }
        case String::Encoding::UTF16: {
            // 直接输出 UTF-16 编码的原始字节
            os.write(reinterpret_cast<const char*>(str.m_impl->m_data.get()), str.m_impl->m_size * sizeof(char16_t));
            break;
        }
        case String::Encoding::UTF32: {
            auto utf32 = Unicode::Convert::Utf16ToUtf32(std::u16string(str.m_impl->m_data.get(), str.m_impl->m_size)); // 输出用 UTF-32 单元串
            os.write(reinterpret_cast<const char*>(utf32.data()), utf32.size() * sizeof(char32_t));
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for output");
        }
        return os;
    }

    std::istream& operator>>(std::istream& is, String& str) {
        std::string input; // 从窄流读取的一行原始字节
        std::getline(is, input);
        str = String(input, str.m_impl->m_encoding);
        return is;
    }

    std::wostream& operator<<(std::wostream& os, const String& str) {
        std::wstring output = str.ToWString(); // 宽流输出缓冲
        os << output;
        return os;
    }

    std::wistream& operator>>(std::wistream& is, String& str) {
        std::wstring input; // 从宽流读取的一行字符
        std::getline(is, input);
        str = String(input);
        return is;
    }

    String String::SubString(size_t index, size_t count) const {
        const size_t total = Size(); // 当前字符串 code point 总数
        if (count == 0 || index >= total) return String();
        size_t start = CodePointOffset(index); // 子串起点 UTF-16 偏移
        size_t end = (count >= total - index) ? m_impl->m_size : CodePointOffset(index + count); // 子串终点 UTF-16 偏移

        size_t newLength = end - start; // 子串 UTF-16 code unit 数
        auto newData = std::make_unique<char16_t[]>(newLength + 1); // 子串独立缓冲区
        std::memcpy(newData.get(), m_impl->m_data.get() + start, newLength * sizeof(char16_t));
        // 子串缓冲单独 NUL 终止，不共享原字符串内存。
        newData[newLength] = u'\0';

        String result; // 返回用子串对象
        result.m_impl->m_size = newLength;
        result.m_impl->m_capacity = newLength;
        result.m_impl->m_data = std::move(newData);
        result.m_impl->m_encoding = m_impl->m_encoding; // 或保持原始 encoding
        return result;
    }

    String String::Left(size_t count) const {
        if (count == 0) return String();
        size_t totalSize = Size(); // 当前 code point 总数
        if (count >= totalSize) return *this;
        // Left/Right 均保持 code point 语义，再复用 SubString 切 UTF-16。
        return SubString(0, count);
    }

    String String::Right(size_t count) const {
        if (count == 0) return String();
        size_t totalSize = Size(); // 当前 code point 总数
        if (count >= totalSize) return *this;
        return SubString(totalSize - count, count);
    }

    String String::ToUpper() const {
        if (Empty()) return String();

        auto buf = std::make_unique<char16_t[]>(m_impl->m_size * 2 + 1); // 最多扩大两倍
        size_t i = 0; // 原始 UTF-16 读取偏移
        size_t j = 0; // 结果 UTF-16 写入偏移

        while (i < m_impl->m_size) {
            char16_t c = m_impl->m_data[i]; // 当前待转换的 UTF-16 单元

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_impl->m_size && m_impl->m_data[i + 1] >= 0xDC00 && m_impl->m_data[i + 1] <= 0xDFFF) {
                // 将代理对还原为补充平面 code point 后再查大写映射。
                char16_t high = c; // 当前代理对高代理
                char16_t low = m_impl->m_data[i + 1]; // 当前代理对低代理
                uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00); // 还原后的 Unicode code point
                uint32_t upperCp = Unicode::Case::SMPToUpper(cp); // 大写映射后的 code point

                // 转回 UTF-16
                if (upperCp <= 0xFFFF) {
                    buf[j++] = static_cast<char16_t>(upperCp);
                }
                else {
                    upperCp -= 0x10000;
                    // 大写结果落在补充平面时重新编码为代理对。
                    buf[j++] = static_cast<char16_t>((upperCp >> 10) + 0xD800);
                    buf[j++] = static_cast<char16_t>((upperCp & 0x3FF) + 0xDC00);
                }

                i += 2;
            }
            else {
                // BMP 单元可以直接通过 BMP 映射表转换。
                buf[j++] = Unicode::Case::BMPToUpper(c);
                ++i;
            }
        }

        buf[j] = u'\0';
        String result; // 大写转换结果对象
        result.m_impl->m_size = j;
        result.m_impl->m_capacity = m_impl->m_size * 2;
        // 结果对象直接接管转换缓冲，避免再复制一遍。
        result.m_impl->m_data = std::move(buf);
        return result;
    }

    String String::ToLower() const {
        if (Empty()) return String();

        // 分配两倍空间，保证 SMP 扩展不会越界
        auto buf = std::make_unique<char16_t[]>(m_impl->m_size * 2 + 1); // 小写转换结果缓冲区
        size_t i = 0; // 原始 UTF-16 读取偏移
        size_t j = 0; // 结果 UTF-16 写入偏移

        while (i < m_impl->m_size) {
            char16_t c = m_impl->m_data[i]; // 当前待转换的 UTF-16 单元

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_impl->m_size && m_impl->m_data[i + 1] >= 0xDC00 && m_impl->m_data[i + 1] <= 0xDFFF) {
                char16_t high = c; // 当前代理对高代理
                char16_t low = m_impl->m_data[i + 1]; // 当前代理对低代理
                uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00); // 还原后的 Unicode code point

                uint32_t lowerCp = Unicode::Case::SMPToLower(cp); // 小写映射后的 code point

                if (lowerCp <= 0xFFFF) {
                    buf[j++] = static_cast<char16_t>(lowerCp);
                }
                else {
                    lowerCp -= 0x10000;
                    // 小写结果落在补充平面时重新编码为代理对。
                    buf[j++] = static_cast<char16_t>((lowerCp >> 10) + 0xD800);
                    buf[j++] = static_cast<char16_t>((lowerCp & 0x3FF) + 0xDC00);
                }

                i += 2;
            }
            else {
                // BMP 单元可以直接通过 BMP 映射表转换。
                buf[j++] = Unicode::Case::BMPToLower(c);
                ++i;
            }
        }

        buf[j] = u'\0';

        String result; // 小写转换结果对象
        result.m_impl->m_size = j;
        result.m_impl->m_capacity = m_impl->m_size * 2;
        // 结果对象直接接管转换缓冲，避免再复制一遍。
        result.m_impl->m_data = std::move(buf);
        return result;
    }


    // 原地转换
    void String::ToUpperInPlace() { *this = ToUpper(); }
    void String::ToLowerInPlace() { *this = ToLower(); }

    size_t String::Find(const String& str, size_t start) const {
        if (str.m_impl->m_size == 0) {
            const size_t total = Size(); // 空模式串按 code point 边界返回起点
            return start <= total ? start : npos;
        }
        if (m_impl->m_size == 0 || str.m_impl->m_size > m_impl->m_size) return npos;
        // 搜索使用 UTF-16 字节级匹配，再用边界检查保证 code point 语义。
        const char16_t* data = m_impl->m_data.get(); // 当前字符串 UTF-16 缓冲区
        const char16_t* patData = str.m_impl->m_data.get(); // 模式串 UTF-16 缓冲区
        const size_t dataSize = m_impl->m_size; // 当前字符串 UTF-16 code unit 数
        const size_t patSize = str.m_impl->m_size; // 模式串 UTF-16 code unit 数
        const size_t startOffset = start == 0 ? 0 : OffsetFromCodePointIndexLinear(data, dataSize, start); // 起始 code point 对应的 UTF-16 偏移
        if (startOffset >= dataSize || dataSize - startOffset < patSize) return npos;

        const char16_t first = patData[0]; // 模式串首单元，用于快速跳过不可能位置
        size_t currentOffset = startOffset; // 当前搜索起点 UTF-16 偏移
        while (currentOffset + patSize <= dataSize) {
            const size_t remaining = dataSize - currentOffset; // 当前剩余可搜索 UTF-16 单元数
            const char16_t* found = std::char_traits<char16_t>::find(data + currentOffset, remaining, first); // 查找首单元候选位置
            if (!found) return npos;
            const size_t offset = static_cast<size_t>(found - data); // 匹配位置 UTF-16 偏移
            if (offset + patSize <= dataSize &&
                IsCodePointBoundary(data, dataSize, offset) &&
                IsCodePointBoundary(data, dataSize, offset + patSize) &&
                std::memcmp(data + offset, patData, patSize * sizeof(char16_t)) == 0) {
                return CountCodePointsBeforeOffset(data, dataSize, offset);
            }
            currentOffset = offset + 1;
        }
        return npos;
    }

    size_t String::LastFind(const String& str, size_t start) const {
        size_t N = Size(); // 当前字符串 code point 数
        size_t M = str.Size(); // 模式串 code point 数
        if (M == 0) return start < N ? start : N;
        if (M > N) return npos;
        if (str.m_impl->m_size == 0) return start < N ? start : N;
        // 反向查找需要 code point 起点映射，因此先确保偏移缓存可用。
        UpdateCodePointCache();
        const size_t maxStartCp = start < (N - M) ? start : (N - M); // 允许反向搜索的最大 code point 起点
        const size_t maxStartOffset = CodePointOffset(maxStartCp); // 最大起点对应的 UTF-16 偏移
        const char16_t* data = m_impl->m_data.get(); // 当前字符串 UTF-16 缓冲区
        const char16_t* patData = str.m_impl->m_data.get(); // 模式串 UTF-16 缓冲区
        const size_t dataSize = m_impl->m_size; // 当前字符串 UTF-16 code unit 数
        const size_t patSize = str.m_impl->m_size; // 模式串 UTF-16 code unit 数
        if (patSize <= dataSize) {
            size_t limit = std::min(maxStartOffset, dataSize - patSize); // 反向搜索的最大 UTF-16 起点
            for (size_t offset = limit + 1; offset-- > 0;) {
                if (IsCodePointBoundary(data, dataSize, offset) &&
                    IsCodePointBoundary(data, dataSize, offset + patSize) &&
                    std::memcmp(data + offset, patData, patSize * sizeof(char16_t)) == 0) {
                    return CountCodePointsBeforeOffset(data, dataSize, offset);
                }
                if (offset == 0) break;
            }
        }
        return npos;
    }

    bool String::StartsWith(const String& str) const {
        if (str.m_impl->m_size == 0) return true;
        if (str.m_impl->m_size > m_impl->m_size) return false;
        if (!IsCodePointBoundary(m_impl->m_data.get(), m_impl->m_size, str.m_impl->m_size)) return false;
        return std::memcmp(m_impl->m_data.get(), str.m_impl->m_data.get(), str.m_impl->m_size * sizeof(char16_t)) == 0;
    }

    bool String::EndsWith(const String& str) const {
        if (str.m_impl->m_size == 0) return true;
        if (str.m_impl->m_size > m_impl->m_size) return false;
        const size_t unitOffset = m_impl->m_size - str.m_impl->m_size; // 后缀候选的 UTF-16 起点偏移
        if (!IsCodePointBoundary(m_impl->m_data.get(), m_impl->m_size, unitOffset)) return false;
        return std::memcmp(m_impl->m_data.get() + unitOffset, str.m_impl->m_data.get(), str.m_impl->m_size * sizeof(char16_t)) == 0;
    }

    bool String::EqualsIgnoreCase(const String& other) const {
        if (Size() != other.Size()) return false;

        size_t len = Size(); // 需要比较的 code point 数
        for (size_t i = 0; i < len; ++i) {
            char32_t c1 = At(i); // 当前字符串第 i 个 code point
            char32_t c2 = other.At(i); // 对比字符串第 i 个 code point

            // 转大写比较
            if (c1 <= 0xFFFF) c1 = Unicode::Case::BMPToUpper(static_cast<char16_t>(c1));
            else c1 = Unicode::Case::SMPToUpper(c1);

            if (c2 <= 0xFFFF) c2 = Unicode::Case::BMPToUpper(static_cast<char16_t>(c2));
            else c2 = Unicode::Case::SMPToUpper(c2);

            if (c1 != c2) return false;
        }
        return true;
    }

    bool String::operator==(const String& other) const {
        if (m_impl->m_size != other.m_impl->m_size) return false;
        return std::memcmp(m_impl->m_data.get(), other.m_impl->m_data.get(), m_impl->m_size * sizeof(char16_t)) == 0;
    }

    bool String::operator!=(const String& other) const {
        return !(*this == other);
    }

    bool String::operator<(const String& other) const {
        return std::lexicographical_compare(
            m_impl->m_data.get(), m_impl->m_data.get() + m_impl->m_size,
            other.m_impl->m_data.get(), other.m_impl->m_data.get() + other.m_impl->m_size);
    }

    bool String::operator<=(const String& other) const { return !(other < *this); }
    bool String::operator>(const String& other) const { return other < *this; }
    bool String::operator>=(const String& other) const { return !(*this < other); }

    std::string String::ToStdString(Encoding enc) const {
        switch (enc) {
        case Encoding::GBK: {
            auto gbk = Unicode::Convert::Utf16ToGbk(std::u16string(m_impl->m_data.get(), m_impl->m_size)); // GBK 输出字节串
            return gbk;
        }
        case Encoding::UTF8: {
            auto u8 = Utf16RangeToUtf8(m_impl->m_data.get(), m_impl->m_size); // UTF-8 输出字节串
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }
        case Encoding::UTF16: {
            // 将 UTF-16 原始数据按字节放入 std::string
            return std::string(reinterpret_cast<const char*>(m_impl->m_data.get()), m_impl->m_size * sizeof(char16_t));
        }
        case Encoding::UTF32: {
            // 先转换为 UTF-32，再按字节放入 std::string
            auto utf32 = Unicode::Convert::Utf16ToUtf32(std::u16string(m_impl->m_data.get(), m_impl->m_size)); // UTF-32 输出单元串
            return std::string(reinterpret_cast<const char*>(utf32.data()), utf32.size() * sizeof(char32_t));
        }
        default:
            throw std::runtime_error("Unsupported encoding for ToStdString");
        }
    }

    std::u8string String::ToU8String() const {
        return Utf16RangeToUtf8(m_impl->m_data.get(), m_impl->m_size);
    }

    std::wstring String::ToWString() const {
        std::wstring ws; // 平台 wchar_t 宽字符串结果

#if WCHAR_MAX == 0xFFFF  // Windows wchar_t=16位
        // Windows 下直接复用 UTF-16 单元构造 wstring。
        ws.assign(m_impl->m_data.get(), m_impl->m_data.get() + m_impl->m_size);
#else  // Linux wchar_t=32位
        for (size_t i = 0; i < m_impl->m_size; ) {
            char16_t c = m_impl->m_data[i]; // 当前 UTF-16 单元
            uint32_t cp; // 当前转换出的 Unicode code point

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_impl->m_size && m_impl->m_data[i + 1] >= 0xDC00 && m_impl->m_data[i + 1] <= 0xDFFF) {
                // POSIX 宽字符常为 UTF-32，需要合并代理对。
                cp = 0x10000 + ((c - 0xD800) << 10) + (m_impl->m_data[i + 1] - 0xDC00);
                i += 2;
            } else {
                cp = c;
                ++i;
            }
            ws.push_back(static_cast<wchar_t>(cp));
        }
#endif
        return ws;
    }

    std::u16string String::ToU16String() const {
        return std::u16string(m_impl->m_data.get(), m_impl->m_size);
    }

    std::u32string String::ToU32String() const {
        return Unicode::Convert::Utf16ToUtf32(std::u16string(m_impl->m_data.get(), m_impl->m_size));
    }

    std::vector<String> String::Split(const String& sep) const {
        std::vector<String> result; // 切分后的片段列表

        if (sep.Empty()) {
            result.push_back(*this);
            return result;
        }

        size_t start = 0;       // 当前段落的起始偏移（UTF-16 单元）
        size_t i = 0;           // 遍历偏移

        while (i < m_impl->m_size) {
            // 尝试匹配分隔符
            bool matched = true; // 当前 UTF-16 偏移是否命中分隔符
            if (i + sep.m_impl->m_size <= m_impl->m_size) {
                // 逐 UTF-16 单元比较分隔符，命中后再切片。
                for (size_t j = 0; j < sep.m_impl->m_size; ++j) {
                    if (m_impl->m_data[i + j] != sep.m_impl->m_data[j]) {
                        matched = false;
                        break;
                    }
                }
            }
            else {
                matched = false;
            }

            if (matched) {
                size_t subLength = i - start; // 当前片段 UTF-16 code unit 数
                auto subData = std::make_unique<char16_t[]>(subLength + 1); // 当前片段独立缓冲区
                std::memcpy(subData.get(), m_impl->m_data.get() + start, subLength * sizeof(char16_t));
                subData[subLength] = u'\0';

                String part; // 当前切分片段对象
                part.m_impl->m_size = subLength;
                part.m_impl->m_capacity = subLength;
                part.m_impl->m_data = std::move(subData);
                // 片段已经拥有独立缓冲，可直接移动进结果数组。
                result.push_back(std::move(part));

                i += sep.m_impl->m_size;
                start = i;
            }
            else {
                // 跳过一个完整 code point
                char16_t c = m_impl->m_data[i]; // 当前待跳过 code point 的首个 UTF-16 单元
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_impl->m_size && m_impl->m_data[i + 1] >= 0xDC00 && m_impl->m_data[i + 1] <= 0xDFFF) {
                    i += 2;
                }
                else {
                    ++i;
                }
            }
        }

        // 添加最后一段
        if (start <= m_impl->m_size) {
            size_t subLength = m_impl->m_size - start; // 最后一段 UTF-16 code unit 数
            auto subData = std::make_unique<char16_t[]>(subLength + 1); // 最后一段独立缓冲区
            std::memcpy(subData.get(), m_impl->m_data.get() + start, subLength * sizeof(char16_t));
            subData[subLength] = u'\0';

            String part; // 最后一段切分片段对象
            part.m_impl->m_size = subLength;
            part.m_impl->m_capacity = subLength;
            part.m_impl->m_data = std::move(subData);
            // 最后一段也保持独立缓冲，避免依赖原字符串生命周期。
            result.push_back(std::move(part));
        }

        return result;
    }

    String String::EscapeJson(const String& str) {
        std::wostringstream woss; // JSON 转义输出缓冲

        for (char32_t c : str) {
            switch (c) {
            case U'"':  woss << L"\\\""; break;
            case U'\\': woss << L"\\\\"; break;
            case U'\b': woss << L"\\b";  break;
            case U'\f': woss << L"\\f";  break;
            case U'\n': woss << L"\\n";  break;
            case U'\r': woss << L"\\r";  break;
            case U'\t': woss << L"\\t";  break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    // 控制字符
                    woss << L"\\u";
                    woss << std::setw(4) << std::setfill(L'0')
                        << std::hex << std::uppercase << static_cast<uint32_t>(c)
                        << std::dec;
                }
                else if (c <= 0xFFFF) {
                    // BMP 直接输出
                    woss << static_cast<wchar_t>(c);
                }
                else {
                    // SMP: 转成 UTF-16 代理对
                    char32_t cp = c - 0x10000; // 代理对计算用补偿码点
                    char16_t high = static_cast<char16_t>(0xD800 + (cp >> 10)); // JSON \u 高代理值
                    char16_t low = static_cast<char16_t>(0xDC00 + (cp & 0x3FF)); // JSON \u 低代理值

                    woss << L"\\u"
                        << std::setw(4) << std::setfill(L'0')
                        << std::hex << std::uppercase << static_cast<uint16_t>(high) << L"\\u"
                        << std::setw(4) << std::setfill(L'0')
                        << std::hex << std::uppercase << static_cast<uint16_t>(low)
                        << std::dec;
                }
            }
        }

        return String(woss.str());
    }

    size_t String::CodePointOffset(size_t index) const {
        UpdateCodePointCache();
        if (index >= m_impl->m_cpOffsets.size()) return m_impl->m_size;
        return m_impl->m_cpOffsets[index];
    }

    void String::InvalidateCodePointCache() {
        const bool countWasValid = m_impl->m_cpCountValid.exchange(false, std::memory_order_acq_rel);     // 失效前 Size() 缓存状态
        const bool offsetsWereValid = m_impl->m_cpOffsetsValid.exchange(false, std::memory_order_acq_rel); // 失效前偏移表缓存状态
        if (!countWasValid && !offsetsWereValid) return;
        std::unique_lock lock(m_impl->m_cpCacheMutex); // 清理偏移表时独占缓存状态
        m_impl->m_cpOffsets.clear();
        m_impl->m_cpCount.store(0, std::memory_order_relaxed);
    }

    void String::UpdateCodePointCount() const {
        {
            std::shared_lock lock(m_impl->m_cpCacheMutex); // 先用共享锁做 O(1) 命中判断
            if (m_impl->m_cpCountValid.load(std::memory_order_acquire)) return;
        }

        std::unique_lock lock(m_impl->m_cpCacheMutex); // 独占刷新 code point 数量缓存
        if (m_impl->m_cpCountValid.load(std::memory_order_acquire)) return;
        size_t count = 0; // 扫描得到的 code point 数
        size_t i = 0;     // 当前 UTF-16 code unit 偏移
        while (i < m_impl->m_size) {
            char16_t c = m_impl->m_data[i]; // 当前 UTF-16 code unit
            if (IsHighSurrogate(c) && i + 1 < m_impl->m_size && IsLowSurrogate(m_impl->m_data[i + 1])) {
                i += 2;
            }
            else {
                ++i;
            }
            ++count;
        }
        m_impl->m_cpCount.store(count, std::memory_order_relaxed);
        m_impl->m_cpCountValid.store(true, std::memory_order_release);
    }

    void String::UpdateCodePointCache() const {
        {
            std::shared_lock lock(m_impl->m_cpCacheMutex); // 先用共享锁做快速命中判断
            if (m_impl->m_cpOffsetsValid.load(std::memory_order_acquire)) return;
        }

        std::unique_lock lock(m_impl->m_cpCacheMutex); // 独占构建完整 offset 表
        if (m_impl->m_cpOffsetsValid.load(std::memory_order_acquire)) return;
        m_impl->m_cpOffsets.clear();
        size_t i = 0; // 当前 UTF-16 code unit 偏移
        while (i < m_impl->m_size) {
            m_impl->m_cpOffsets.push_back(i);
            char16_t c = m_impl->m_data[i]; // 当前 code point 的首个 UTF-16 code unit
            if (IsHighSurrogate(c) && i + 1 < m_impl->m_size && IsLowSurrogate(m_impl->m_data[i + 1]))
                i += 2;
            else
                ++i;
        }
        m_impl->m_cpCount.store(m_impl->m_cpOffsets.size(), std::memory_order_relaxed);
        m_impl->m_cpCountValid.store(true, std::memory_order_release);
        m_impl->m_cpOffsetsValid.store(true, std::memory_order_release);
    }

    String String::FormatAny(const String& fmt, const std::vector<Any>& args) {
        auto& instance = StringFormat::FormatInternal::Instance();
        return instance.FormatAny(fmt, args);
    }

    String String::FormatViews(const String& fmt, const StringFormat::FormatArgView* args, size_t argCount) {
        auto& instance = StringFormat::FormatInternal::Instance();
        return instance.FormatViews(fmt, args, argCount);
    }

    const String& String::CachedFormatString(std::u16string_view fmt) {
        struct Cache {
            std::u16string key; // 上一次格式串字面量内容
            String value;       // 与 key 对应的 String 对象
        };
        thread_local Cache cache; // 每线程一个小缓存，避免跨线程同步
        if (std::u16string_view(cache.key) != fmt) {
            cache.key.assign(fmt);
            cache.value = String(std::u16string_view(cache.key));
        }
        return cache.value;
    }

    const String& String::CachedFormatString(std::u32string_view fmt) {
        struct Cache {
            std::u32string key; // 上一次 UTF-32 格式串字面量内容
            String value;       // 与 key 对应的 String 对象
        };
        thread_local Cache cache; // 每线程一个小缓存，避免跨线程同步
        if (std::u32string_view(cache.key) != fmt) {
            cache.key.assign(fmt);
            cache.value = String(std::u32string_view(cache.key));
        }
        return cache.value;
    }
}
