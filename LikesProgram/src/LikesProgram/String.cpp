#include "../../include/LikesProgram/String.hpp"
#include "../../include/LikesProgram/unicode/Unicode.hpp"
#include <stdexcept>
#include <cwchar>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <array>
#include <iostream>
#include <sstream>
#include <locale>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace LikesProgram {
    String::String()
        : m_data(std::make_unique<char16_t[]>(1)), m_size(0), encoding(Encoding::UTF8) {
        m_data[0] = u'\0';
    }

    String::String(const char* s, Encoding enc): encoding(enc) {
        if (!s) s = "";

        switch (enc) {
        case Encoding::UTF8: {
            size_t len = std::strlen(s);
            auto utf16 = Unicode::Convert::Utf8ToUtf16(
                std::u8string(reinterpret_cast<const char8_t*>(s),
                    reinterpret_cast<const char8_t*>(s) + len)
            );
            m_size = utf16.size();
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        case Encoding::GBK: {
            auto utf16 = Unicode::Convert::GbkToUtf16(std::string(s));
            m_size = utf16.size();
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        case Encoding::UTF16: {
            const char16_t* ps = reinterpret_cast<const char16_t*>(s);
            size_t len = 0;
            while (ps[len] != 0) ++len;
            m_size = len;
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), ps, m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        case Encoding::UTF32: {
            const char32_t* ps = reinterpret_cast<const char32_t*>(s);
            size_t len = 0;
            while (ps[len] != 0) ++len;
            auto utf16 = Unicode::Convert::Utf32ToUtf16(std::u32string(ps, ps + len));
            m_size = utf16.size();
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for const char*");
        }
    }

    String::String(const char8_t* s): encoding(Encoding::UTF8) {
        if (!s) s = u8"";
        auto utf16 = Unicode::Convert::Utf8ToUtf16(std::u8string(s));
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const char16_t* s): encoding(Encoding::UTF16) {
        if (!s) s = u"";
        size_t len = 0;
        while (s[len] != 0) ++len;
        m_size = len;
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), s, m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const char32_t* s): encoding(Encoding::UTF32) {
        if (!s) s = U"";
        size_t len = 0;
        while (s[len] != 0) ++len; // 计算长度
        auto utf16 = Unicode::Convert::Utf32ToUtf16(std::u32string(s, s + len));
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const String& other)
        : m_size(other.m_size), encoding(other.encoding) {
        if (other.m_data) {
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), other.m_data.get(), (m_size + 1) * sizeof(char16_t));
        } else {
            m_data = std::make_unique<char16_t[]>(1);
            m_data[0] = u'\0';
        }
    }

    String::String(String&& other) noexcept
        : m_data(std::move(other.m_data)), m_size(other.m_size), encoding(other.encoding) {
        other.m_size = 0;
    }

    String::String(const char8_t c): encoding(Encoding::UTF8) {
        char8_t buf[2] = { c, 0 };
        auto utf16 = Unicode::Convert::Utf8ToUtf16(std::u8string(buf));
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const char16_t c): encoding(Encoding::UTF16) {
        m_size = 1;
        m_data = std::make_unique<char16_t[]>(2);
        m_data[0] = c;
        m_data[1] = u'\0';
    }

    String::String(const char32_t c): encoding(Encoding::UTF32) {
        auto utf16 = Unicode::Convert::Utf32ToUtf16(std::u32string(1, c));
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    // 析构函数
    String::~String() = default;

    String& String::operator=(const String& other) {
        if (this != &other) {
            auto new_data = other.m_data ? std::make_unique<char16_t[]>(other.m_size + 1)
                : std::make_unique<char16_t[]>(1);
            if (other.m_data) {
                std::memcpy(new_data.get(), other.m_data.get(), (other.m_size + 1) * sizeof(char16_t));
            }
            else {
                new_data[0] = u'\0';
            }
            m_data = std::move(new_data);
            m_size = other.m_size;
            encoding = other.encoding;
        }
        return *this;
    }

    String& String::operator=(String&& other) noexcept {
        if (this != &other) {
            m_size = other.m_size;
            m_data = std::move(other.m_data);
            other.m_size = 0;
            encoding = other.encoding;
        }
        return *this;
    }

    size_t String::Size() const {
        size_t count = 0;
        size_t i = 0;
        while (i < m_size) {
            char16_t c = m_data[i];
            if (c >= 0xD800 && c <= 0xDBFF) { // 高位 surrogate
                if (i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                    i += 2; // 跳过完整 surrogate pair
                }
                else {
                    // 孤立高位 surrogate，当作单字符处理
                    ++i;
                }
            }
            else {
                ++i;
            }
            ++count;
        }
        return count;
    }

    size_t String::Length() const {
        return Size();
    }

    bool String::Empty() const {
        return m_size == 0;
    }

    void String::Clear() {
        m_data = std::make_unique<char16_t[]>(1);
        m_data[0] = u'\0';
        m_size = 0;
    }

    char32_t String::At(size_t index) const {
        update_cp_cache();
        if (index >= cp_offsets.size()) throw std::out_of_range("index out of range");
        size_t i = cp_offsets[index];
        char16_t c = m_data[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            char16_t low = m_data[i + 1];
            return 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        }
        return c;
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
        size_t new_size = m_size + str.m_size;
        auto new_data = std::make_unique<char16_t[]>(new_size + 1);
        if (m_size > 0) std::memcpy(new_data.get(), m_data.get(), m_size * sizeof(char16_t));
        std::memcpy(new_data.get() + m_size, str.m_data.get(), str.m_size * sizeof(char16_t));
        new_data[new_size] = u'\0';
        m_data = std::move(new_data);
        m_size = new_size;
        cp_cache_valid = false;
        return *this;
    }

    String& String::operator+=(const String& str) {
        return Append(str);
    }

    std::ostream& operator<<(std::ostream& os, const String& str) {
        switch (str.encoding) {
        case String::Encoding::GBK: {
            auto gbk = Unicode::Convert::Utf16ToGbk(std::u16string(str.m_data.get(), str.m_size));
            os << gbk;
            break;
        }
        case String::Encoding::UTF8: {
            auto utf8 = Unicode::Convert::Utf16ToUtf8(std::u16string(str.m_data.get(), str.m_size));
            os.write(reinterpret_cast<const char*>(utf8.data()), utf8.size() * sizeof(char8_t));
            break;
        }
        case String::Encoding::UTF16: {
            // 直接输出 UTF-16 编码的原始字节
            os.write(reinterpret_cast<const char*>(str.m_data.get()), str.m_size * sizeof(char16_t));
            break;
        }
        case String::Encoding::UTF32: {
            auto utf32 = Unicode::Convert::Utf16ToUtf32(std::u16string(str.m_data.get(), str.m_size));
            os.write(reinterpret_cast<const char*>(utf32.data()), utf32.size() * sizeof(char32_t));
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for output");
        }
        return os;
    }

    std::istream& operator>>(std::istream& is, String& str) {
        std::string input;
        std::getline(is, input);  // 读取一行输入
        str = String(input, str.encoding);
        return is;
    }

    std::wostream& operator<<(std::wostream& os, const String& str) {
        std::wstring output = str.ToWString();
        os << output;
        return os;
    }

    std::wistream& operator>>(std::wistream& is, String& str) {
        std::wstring input;
        std::getline(is, input);  // 读取一行输入
        str = String(input);
        return is;
    }

    String String::SubString(size_t index, size_t count) const {
        if (count == 0 || index >= Size()) return String(); // 越界或长度为0返回空串

        size_t start = CodePointOffset(index);
        size_t end = (index + count >= Size()) ? m_size : CodePointOffset(index + count);

        size_t new_len = end - start;
        auto new_data = std::make_unique<char16_t[]>(new_len + 1);
        std::memcpy(new_data.get(), m_data.get() + start, new_len * sizeof(char16_t));
        new_data[new_len] = u'\0';

        String result;
        result.m_size = new_len;
        result.m_data = std::move(new_data);
        return result;
    }

    String String::Left(size_t count) const {
        if (count == 0) return String();
        size_t sz = Size();
        if (count >= sz) return *this;
        return SubString(0, count);
    }

    String String::Right(size_t count) const {
        if (count == 0) return String();
        size_t sz = Size();
        if (count >= sz) return *this;
        return SubString(sz - count, count);
    }

    String String::ToUpper() const {
        if (Empty()) return String();

        auto buf = std::make_unique<char16_t[]>(m_size * 2 + 1); // 最多扩大两倍
        size_t i = 0, j = 0;

        while (i < m_size) {
            char16_t c = m_data[i];

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                // SMP
                char16_t high = c;
                char16_t low = m_data[i + 1];
                uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
                uint32_t upper_cp = Unicode::Case::SMPToUpper(cp);

                // 转回 UTF-16
                if (upper_cp <= 0xFFFF) {
                    buf[j++] = static_cast<char16_t>(upper_cp);
                }
                else {
                    upper_cp -= 0x10000;
                    buf[j++] = static_cast<char16_t>((upper_cp >> 10) + 0xD800);
                    buf[j++] = static_cast<char16_t>((upper_cp & 0x3FF) + 0xDC00);
                }

                i += 2;
            }
            else {
                // BMP
                buf[j++] = Unicode::Case::BMPToUpper(c);
                ++i;
            }
        }

        buf[j] = u'\0';
        String result;
        result.m_size = j;
        result.m_data = std::move(buf);
        return result;
    }

    String String::ToLower() const {
        if (Empty()) return String();

        // 分配两倍空间，保证 SMP 扩展不会越界
        auto buf = std::make_unique<char16_t[]>(m_size * 2 + 1);
        size_t i = 0; // 原字符串索引
        size_t j = 0; // 新字符串索引

        while (i < m_size) {
            char16_t c = m_data[i];

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                // SMP surrogate pair
                char16_t high = c;
                char16_t low = m_data[i + 1];
                uint32_t cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);

                uint32_t lower_cp = Unicode::Case::SMPToLower(cp);

                if (lower_cp <= 0xFFFF) {
                    buf[j++] = static_cast<char16_t>(lower_cp);
                }
                else {
                    lower_cp -= 0x10000;
                    buf[j++] = static_cast<char16_t>((lower_cp >> 10) + 0xD800);
                    buf[j++] = static_cast<char16_t>((lower_cp & 0x3FF) + 0xDC00);
                }

                i += 2;
            }
            else {
                // BMP
                buf[j++] = Unicode::Case::BMPToLower(c);
                ++i;
            }
        }

        buf[j] = u'\0';

        String result;
        result.m_size = j;
        result.m_data = std::move(buf);
        return result;
    }


    // 原地转换
    void String::ToUpperInPlace() { *this = ToUpper(); }
    void String::ToLowerInPlace() { *this = ToLower(); }

    size_t String::Find(const String& str, size_t start) const {
        size_t N = Size();
        size_t M = str.Size();
        if (M == 0) return start <= N ? start : npos;
        if (start >= N) return npos;

        // 1. 构建 UTF-32 数组
        std::vector<char32_t> text(N), pat(M);
        for (size_t i = 0; i < N; ++i) text[i] = At(i);
        for (size_t i = 0; i < M; ++i) pat[i] = str.At(i);

        // 2. 构建部分匹配表
        std::vector<size_t> fail(M, 0);
        for (size_t i = 1, j = 0; i < M; ++i) {
            while (j > 0 && pat[i] != pat[j]) j = fail[j - 1];
            if (pat[i] == pat[j]) ++j;
            fail[i] = j;
        }

        // 3. KMP 匹配
        size_t j = 0;
        for (size_t i = start; i < N; ++i) {
            while (j > 0 && text[i] != pat[j]) j = fail[j - 1];
            if (text[i] == pat[j]) ++j;
            if (j == M) return i - M + 1;  // 找到匹配
        }

        return npos;
    }

    size_t String::LastFind(const String& str, size_t start) const {
        size_t N = Size();
        size_t M = str.Size();
        if (M == 0) return start < N ? start : N;
        if (M > N) return npos;
        if (start >= N) start = N - 1;

        // 构建模式数组
        std::vector<char32_t> pat(M);
        for (size_t i = 0; i < M; ++i) pat[i] = str.At(i);

        // 构建部分匹配表
        std::vector<size_t> fail(M, 0);
        for (size_t i = 1, j = 0; i < M; ++i) {
            while (j > 0 && pat[i] != pat[j]) j = fail[j - 1];
            if (pat[i] == pat[j]) ++j;
            fail[i] = j;
        }

        // 倒序匹配
        size_t j = 0;
        for (size_t i = Size() - start; i-- > 0;) { // 注意 size_t 下溢写法
            while (j > 0 && At(i) != pat[M - 1 - j]) j = fail[j - 1];
            if (At(i) == pat[M - 1 - j]) ++j;
            if (j == M) return i; // 匹配成功，返回正向索引
        }

        return npos;
    }

    bool String::StartsWith(const String& str) const {
        size_t this_size = Size(); // code point 数量
        size_t str_size = str.Size();
        if (str_size == 0) return true;
        if (str_size > this_size) return false;

        for (size_t i = 0; i < str_size; ++i) {
            if (At(i) != str.At(i)) return false;
        }
        return true;
    }

    bool String::EndsWith(const String& str) const {
        size_t this_size = Size(); // code point 数量
        size_t str_size = str.Size();
        if (str_size == 0) return true;
        if (str_size > this_size) return false;

        size_t offset = this_size - str_size;
        for (size_t i = 0; i < str_size; ++i) {
            if (At(offset + i) != str.At(i)) return false;
        }
        return true;
    }

    bool String::EqualsIgnoreCase(const String& other) const {
        if (Size() != other.Size()) return false;

        size_t len = Size();
        for (size_t i = 0; i < len; ++i) {
            char32_t c1 = At(i);
            char32_t c2 = other.At(i);

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
        if (m_size != other.m_size) return false;
        for (size_t i = 0; i < m_size; ++i) {
            if (m_data[i] != other.m_data[i]) return false;
        }
        return true;
    }

    bool String::operator!=(const String& other) const {
        return !(*this == other);
    }

    bool String::operator<(const String& other) const {
        size_t len = std::min(Size(), other.Size());
        for (size_t i = 0; i < len; ++i) {
            char32_t c1 = At(i);
            char32_t c2 = other.At(i);
            if (c1 < c2) return true;
            if (c1 > c2) return false;
        }
        return Size() < other.Size();
    }

    bool String::operator<=(const String& other) const { return !(other < *this); }
    bool String::operator>(const String& other) const { return other < *this; }
    bool String::operator>=(const String& other) const { return !(*this < other); }

    std::string String::ToStdString(Encoding enc) const {
        switch (enc) {
        case Encoding::GBK: {
            auto gbk = Unicode::Convert::Utf16ToGbk(std::u16string(m_data.get(), m_size));
            return gbk;
        }
        case Encoding::UTF8: {
            auto u8 = Unicode::Convert::Utf16ToUtf8(std::u16string(m_data.get(), m_size));
            return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
        }
        case Encoding::UTF16: {
            // 将 UTF-16 原始数据按字节放入 std::string
            return std::string(reinterpret_cast<const char*>(m_data.get()), m_size * sizeof(char16_t));
        }
        case Encoding::UTF32: {
            // 先转换为 UTF-32，再按字节放入 std::string
            auto utf32 = Unicode::Convert::Utf16ToUtf32(std::u16string(m_data.get(), m_size));
            return std::string(reinterpret_cast<const char*>(utf32.data()), utf32.size() * sizeof(char32_t));
        }
        default:
            throw std::runtime_error("Unsupported encoding for ToStdString");
        }
    }

    std::wstring String::ToWString() const {
        std::wstring ws;

#if WCHAR_MAX == 0xFFFF  // Windows wchar_t=16位
        ws.assign(m_data.get(), m_data.get() + m_size);
#else  // Linux wchar_t=32位
        for (size_t i = 0; i < m_size; ) {
            char16_t c = m_data[i];
            uint32_t cp;

            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                // SMP surrogate pair
                cp = 0x10000 + ((c - 0xD800) << 10) + (m_data[i + 1] - 0xDC00);
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
        return std::u16string(m_data.get(), m_size);
    }

    std::u32string String::ToU32String() const {
        return Unicode::Convert::Utf16ToUtf32(std::u16string(m_data.get(), m_size));
    }

    String::String(const std::string& s, Encoding enc): encoding(enc) {
        switch (enc) {
        case Encoding::UTF8: {
            auto utf16 = Unicode::Convert::Utf8ToUtf16(
                std::u8string(reinterpret_cast<const char8_t*>(s.data()),
                    reinterpret_cast<const char8_t*>(s.data() + s.size()))
            );
            m_size = utf16.size();
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        case Encoding::GBK: {
            auto utf16 = Unicode::Convert::GbkToUtf16(s);
            m_size = utf16.size();
            m_data = std::make_unique<char16_t[]>(m_size + 1);
            std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
            m_data[m_size] = u'\0';
            break;
        }
        default:
            throw std::runtime_error("Unsupported encoding for std::string");
        }
    }

    String::String(const std::u8string& s): encoding(Encoding::UTF8) {
        auto utf16 = Unicode::Convert::Utf8ToUtf16(s);
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const std::wstring& s) : encoding(Encoding::UTF16) {
#if WCHAR_MAX == 0xFFFF  // Windows
        m_size = s.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), s.data(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
#else  // Linux
        std::u32string tmp;
        for (wchar_t c : s) tmp.push_back(static_cast<char32_t>(c));
        auto utf16 = Unicode::Convert::Utf32ToUtf16(tmp);
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
#endif
    }

    String::String(const std::u16string& s) : encoding(Encoding::UTF16) {
        m_size = s.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), s.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    String::String(const std::u32string& s): encoding(Encoding::UTF32) {
        auto utf16 = Unicode::Convert::Utf32ToUtf16(s);
        m_size = utf16.size();
        m_data = std::make_unique<char16_t[]>(m_size + 1);
        std::memcpy(m_data.get(), utf16.c_str(), m_size * sizeof(char16_t));
        m_data[m_size] = u'\0';
    }

    std::vector<String> String::Split(const String& sep) const {
        std::vector<String> result;

        if (sep.Empty()) {
            result.push_back(*this);
            return result;
        }

        size_t start = 0;       // 当前段落的起始偏移（UTF-16 单元）
        size_t i = 0;           // 遍历偏移

        while (i < m_size) {
            // 尝试匹配分隔符
            bool matched = true;
            if (i + sep.m_size <= m_size) {
                for (size_t j = 0; j < sep.m_size; ++j) {
                    if (m_data[i + j] != sep.m_data[j]) {
                        matched = false;
                        break;
                    }
                }
            }
            else {
                matched = false;
            }

            if (matched) {
                size_t sub_len = i - start;
                auto sub_data = std::make_unique<char16_t[]>(sub_len + 1);
                std::memcpy(sub_data.get(), m_data.get() + start, sub_len * sizeof(char16_t));
                sub_data[sub_len] = u'\0';

                String part;
                part.m_size = sub_len;
                part.m_data = std::move(sub_data);
                result.push_back(std::move(part));

                i += sep.m_size;
                start = i;
            }
            else {
                // 跳过一个完整 code point
                char16_t c = m_data[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                    i += 2; // surrogate pair
                }
                else {
                    ++i;
                }
            }
        }

        // 添加最后一段
        if (start <= m_size) {
            size_t sub_len = m_size - start;
            auto sub_data = std::make_unique<char16_t[]>(sub_len + 1);
            std::memcpy(sub_data.get(), m_data.get() + start, sub_len * sizeof(char16_t));
            sub_data[sub_len] = u'\0';

            String part;
            part.m_size = sub_len;
            part.m_data = std::move(sub_data);
            result.push_back(std::move(part));
        }

        return result;
    }

    size_t String::CodePointOffset(size_t index) const {
        size_t i = 0;  // UTF-16 偏移
        size_t cp = 0; // Unicode code point
        while (i < m_size && cp < index) {
            char16_t c = m_data[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF) {
                i += 2; // surrogate pair
            }
            else {
                ++i;
            }
            ++cp;
        }
        return i;
    }

    void String::update_cp_cache() const {
        if (cp_cache_valid) return;
        cp_offsets.clear();
        size_t i = 0;
        while (i < m_size) {
            cp_offsets.push_back(i);
            char16_t c = m_data[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < m_size && m_data[i + 1] >= 0xDC00 && m_data[i + 1] <= 0xDFFF)
                i += 2; // surrogate pair
            else
                ++i;
        }
        cp_cache_valid = true;
    }
}