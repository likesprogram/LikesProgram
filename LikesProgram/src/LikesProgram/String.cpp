//#include "../../include/LikesProgram/String.hpp"
//#include <stdexcept>
//#include <cwchar>
//#include <atomic>
//#include <algorithm>
//#include <cstring>
//#include <array>
//#ifdef _WIN32
//#define NOMINMAX
//#include <windows.h>
//#endif
//
//namespace LikesProgram {
//    String::String() : m_data(new StringData(u"", 0)) {
//        m_data->encoding = Encoding::UTF8;
//        m_data->lengthCache = 0;
//        m_data->lengthValid = true;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char* s, Encoding enc) {
//        if (!s) s = "";
//        switch (enc) {
//        case Encoding::UTF8: {
//            auto utf16 = Utf8ToUtf16(std::string(s));
//            m_data = new StringData(utf16.c_str(), utf16.size());
//            m_data->encoding = Encoding::UTF8;
//            break;
//        }
//        case Encoding::GBK: {
//            auto utf16 = GbkToUtf16(std::string(s));
//            m_data = new StringData(utf16.c_str(), utf16.size());
//            m_data->encoding = Encoding::GBK;
//            break;
//        }
//        case Encoding::UTF16: {
//            size_t len = 0;
//            const char16_t* ps = reinterpret_cast<const char16_t*>(s);
//            while (ps[len] != 0) len++;
//            m_data = new StringData(ps, len);
//            m_data->encoding = Encoding::UTF16;
//            break;
//        }
//        case Encoding::UTF32: {
//            size_t len = 0;
//            const char32_t* ps = reinterpret_cast<const char32_t*>(s);
//            while (ps[len] != 0) len++;
//            auto utf16 = Utf32ToUtf16(std::u32string(ps, ps + len));
//            m_data = new StringData(utf16.c_str(), utf16.size());
//            m_data->encoding = Encoding::UTF32;
//            break;
//        }
//        default:
//            throw std::runtime_error("Unsupported encoding for const char*");
//        }
//
//        m_data->lengthCache = 0;
//        m_data->lengthValid = false;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char8_t* s) {
//        if (!s) s = u8"";
//        auto utf16 = Utf8ToUtf16(reinterpret_cast<const char*>(s));
//        m_data = new StringData(utf16.c_str(), utf16.size());
//        m_data->encoding = Encoding::UTF8;
//        m_data->lengthCache = 0;
//        m_data->lengthValid = false;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char16_t* s) {
//        if (!s) s = u"";
//        size_t len = 0;
//        while (s[len] != 0) len++;
//        m_data = new StringData(s, len);
//        m_data->encoding = Encoding::UTF16;
//        m_data->lengthCache = 0;
//        m_data->lengthValid = false;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char32_t* s) {
//        if (!s) s = U"";
//        size_t len = 0;
//        while (s[len] != 0) len++;
//        auto utf16 = Utf32ToUtf16(std::u32string(s, s + len));
//        m_data = new StringData(utf16.c_str(), utf16.size());
//        m_data->encoding = Encoding::UTF32;
//        m_data->lengthCache = 0;
//        m_data->lengthValid = false;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const String& other) : m_data(other.m_data) {
//        if (m_data) m_data->refCount.fetch_add(1, std::memory_order_relaxed);
//    }
//
//    String::String(String&& other) noexcept : m_data(other.m_data) {
//        other.m_data = nullptr;
//    }
//
//    String::String(const char c) {
//        char s[2] = { c, 0 };
//        auto utf16 = Utf8ToUtf16(std::string(s));
//        m_data = new StringData(utf16.c_str(), utf16.size());
//        m_data->encoding = Encoding::UTF8;
//        m_data->lengthCache = 1;
//        m_data->lengthValid = true;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char16_t c) {
//        m_data = new StringData(&c, 1);
//        m_data->encoding = Encoding::UTF16;
//        m_data->lengthCache = 1;
//        m_data->lengthValid = true;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::String(const char32_t c) {
//        auto utf16 = Utf32ToUtf16(std::u32string(1, c));
//        m_data = new StringData(utf16.c_str(), utf16.size());
//        m_data->encoding = Encoding::UTF32;
//        m_data->lengthCache = 1;
//        m_data->lengthValid = true;
//        m_data->hashCache = 0;
//        m_data->hashValid = false;
//    }
//
//    String::~String() {
//        if (!m_data) return;
//        if (m_data && m_data->refCount.fetch_sub(1) == 1) {
//            delete m_data;
//        }
//    }
//
//    String& String::operator=(const String& other) {
//        if (this != &other) {              // 防止自赋值
//            // 增加 new 对象引用计数
//            other.m_data->refCount.fetch_add(1);
//
//            // 释放当前对象持有的资源
//            if (m_data && m_data->refCount.fetch_sub(1) == 1) {
//                delete m_data;
//            }
//
//            // 指向新的数据
//            m_data = other.m_data;
//        }
//        return *this;
//    }
//
//    String& String::operator=(String&& other) noexcept {
//        if (this != &other) {
//            // 释放当前对象资源
//            if (m_data && m_data->refCount.fetch_sub(1) == 1) {
//                delete m_data;
//            }
//
//            // 转移资源所有权
//            m_data = other.m_data;
//            other.m_data = nullptr;   // 避免 double delete
//        }
//        return *this;
//    }
//
//    size_t String::Size() const {
//        if (!m_data) return npos;
//        if (m_data->lengthValid) return m_data->lengthCache;
//
//        // 计算字符数（UTF-16 码点计数，处理 surrogate pair）
//        size_t count = 0;
//        for (size_t i = 0; i < m_data->size; ++i) {
//            char16_t c = m_data->data[i];
//            if (c >= 0xD800 && c <= 0xDBFF) {
//                // 高代理项，和下一个低代理项组成一个 Unicode 字符
//                if (i + 1 < m_data->size) {
//                    char16_t c2 = m_data->data[i + 1];
//                    if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
//                        ++i; // 跳过低代理项
//                    }
//                }
//            }
//            ++count;
//        }
//
//        m_data->lengthCache = count;
//        m_data->lengthValid = true;
//        return count;
//    }
//
//    size_t String::Length() const {
//        return Size();
//    }
//
//    bool String::Empty() const {
//        return m_data == nullptr || m_data->size == 0;
//    }
//
//    void String::Clear() {
//        Detach();  // COW 分离逻辑
//        if (m_data) {
//            delete[] m_data->data;
//            m_data->data = new char16_t[1] {0};
//            m_data->size = 0;
//            m_data->capacity = 0;
//            m_data->lengthCache = 0;
//            m_data->lengthValid = true;
//            m_data->hashCache = 0;
//            m_data->hashValid = true;
//        }
//    }
//
//    void String::Reserve(size_t size) {
//        Detach();  // COW 分离逻辑
//        if (!m_data) {
//            m_data = new StringData(u"", 0);
//        }
//        if (size <= m_data->capacity) return;
//
//        char16_t* newData = new char16_t[size + 1];
//        if (m_data->size > 0) {
//            std::memcpy(newData, m_data->data, m_data->size * sizeof(char16_t));
//        }
//        newData[m_data->size] = 0;
//
//        delete[] m_data->data;
//        m_data->data = newData;
//        m_data->capacity = size;
//    }
//
//    char32_t String::operator[](size_t index) const {
//        if (!m_data) throw std::out_of_range("String is null");
//        size_t pos = 0;
//        size_t i = 0;
//
//        while (i < m_data->size) {
//            char16_t c = m_data->data[i];
//
//            // 处理代理项
//            if (c >= 0xD800 && c <= 0xDBFF) { // 高代理
//                if (i + 1 >= m_data->size)
//                    throw std::runtime_error("Invalid UTF-16: high surrogate without low surrogate");
//                char16_t low = m_data->data[i + 1];
//                if (low < 0xDC00 || low > 0xDFFF)
//                    throw std::runtime_error("Invalid UTF-16: bad surrogate pair");
//
//                if (pos == index) {
//                    char32_t cp = 0x10000 + (((c - 0xD800) << 10) | (low - 0xDC00));
//                    return cp;
//                }
//                i += 2;
//                ++pos;
//            }
//            else { // 非代理项
//                if (pos == index) return static_cast<char32_t>(c);
//                ++i;
//                ++pos;
//            }
//        }
//
//        throw std::out_of_range("String index out of range");
//    }
//
//    char32_t String::At(size_t index) const {
//        size_t len = Length();
//        if (index >= len) {
//            throw std::out_of_range("String::At - index out of range");
//        }
//        return (*this)[index];
//    }
//
//    char32_t String::Front() const {
//        if (!m_data || m_data->size == 0)
//            throw std::out_of_range("String is empty");
//
//        char16_t c = m_data->data[0];
//        if (c >= 0xD800 && c <= 0xDBFF) {   // 代理对
//            if (m_data->size < 2)
//                throw std::runtime_error("Invalid UTF-16: high surrogate without low surrogate");
//            char16_t low = m_data->data[1];
//            if (low < 0xDC00 || low > 0xDFFF)
//                throw std::runtime_error("Invalid UTF-16 surrogate pair");
//            return 0x10000 + (((c - 0xD800) << 10) | (low - 0xDC00));
//        }
//        return static_cast<char32_t>(c);
//    }
//
//    char32_t String::Back() const {
//        if (!m_data || m_data->size == 0)
//            throw std::out_of_range("String is empty");
//
//        size_t i = m_data->size;
//        // 从尾部向前找最后一个码点
//        while (i > 0) {
//            char16_t c = m_data->data[i - 1];
//            if (c >= 0xDC00 && c <= 0xDFFF) {  // 低代理项
//                if (i < 2)
//                    throw std::runtime_error("Invalid UTF-16: low surrogate without high surrogate");
//                char16_t high = m_data->data[i - 2];
//                if (high < 0xD800 || high > 0xDBFF)
//                    throw std::runtime_error("Invalid UTF-16 surrogate pair");
//                return 0x10000 + (((high - 0xD800) << 10) | (c - 0xDC00));
//            }
//            else {
//                return static_cast<char32_t>(c);
//            }
//        }
//
//        throw std::out_of_range("String is empty");
//    }
//
//    String& String::Append(const String& str) {
//        if (str.Empty()) return *this;
//
//        if (!m_data) m_data = new StringData(u"", 0);
//
//        Detach();  // COW 分离逻辑
//        EnsureCapacity(m_data->size + str.m_data->size);
//
//        std::memcpy(m_data->data + m_data->size, str.m_data->data,
//            str.m_data->size * sizeof(char16_t));
//
//        m_data->size += str.m_data->size;
//        m_data->data[m_data->size] = u'\0';
//
//        m_data->lengthValid = false;
//        m_data->hashValid = false;
//        return *this;
//    }
//
//    String& String::operator+=(const String& str) {
//        return Append(str);
//    }
//
//    String& String::Prepend(const String& str) {
//        if (str.Empty()) return *this;
//
//        if (!m_data) m_data = new StringData(u"", 0);
//
//        Detach();  // COW 分离逻辑
//        EnsureCapacity(m_data->size + str.m_data->size);
//
//        // 把原来的内容整体向后移动
//        std::memmove(
//            m_data->data + str.m_data->size,
//            m_data->data,
//            m_data->size * sizeof(char16_t)
//        );
//
//        // 把要加的字符串拷贝到前面
//        std::memcpy(m_data->data, str.m_data->data,
//            str.m_data->size * sizeof(char16_t));
//
//        m_data->size += str.m_data->size;
//        m_data->data[m_data->size] = u'\0';
//
//        m_data->lengthValid = false;
//        m_data->hashValid = false;
//        return *this;
//    }
//
//    String& String::Insert(size_t index, const String& str) {
//        if (str.Empty()) return *this;
//
//        if (!m_data) m_data = new StringData(u"", 0);
//
//        if (index > m_data->size) throw std::out_of_range("Insert index");
//
//        Detach();  // COW 分离逻辑
//        EnsureCapacity(m_data->size + str.m_data->size);
//
//        // 移动 [index, end) 部分
//        std::memmove(
//            m_data->data + index + str.m_data->size,
//            m_data->data + index,
//            (m_data->size - index) * sizeof(char16_t)
//        );
//
//        // 拷贝新内容
//        std::memcpy(
//            m_data->data + index,
//            str.m_data->data,
//            str.m_data->size * sizeof(char16_t)
//        );
//
//        m_data->size += str.m_data->size;
//        m_data->data[m_data->size] = u'\0';
//
//        m_data->lengthValid = false;
//        m_data->hashValid = false;
//        return *this;
//    }
//
//    String& String::Remove(size_t index, size_t count) {
//        if (!m_data || index >= m_data->size) return *this;
//
//        if (index + count > m_data->size) count = m_data->size - index;
//
//        Detach();
//
//        std::memmove(
//            m_data->data + index,
//            m_data->data + index + count,
//            (m_data->size - index - count) * sizeof(char16_t)
//        );
//
//        m_data->size -= count;
//        m_data->data[m_data->size] = u'\0';
//
//        m_data->lengthValid = false;
//        m_data->hashValid = false;
//        return *this;
//    }
//
//    String& String::Replace(size_t index, size_t count, const String& str) {
//        Remove(index, count);
//        return Insert(index, str);
//    }
//
//    String String::SubString(size_t index, size_t count) const {
//        if (!m_data || index >= m_data->size) return String();
//
//        size_t realCount = std::min(count, m_data->size - index);
//        return String(std::u16string(m_data->data + index, realCount));
//    }
//
//    String String::Left(size_t count) const {
//        if (!m_data) return String();
//        return SubString(0, std::min(count, m_data->size));
//    }
//
//    String String::Right(size_t count) const {
//        if (!m_data) return String();
//        size_t n = m_data->size;
//        if (count >= n) return *this;
//        return SubString(n - count, count);
//    }
//
//    String String::ToUpper() const {
//        if (!m_data) return String();
//        std::u16string result;
//        result.reserve(m_data->size);
//
//        for (size_t i = 0; i < m_data->size; ++i) {
//            char16_t c = m_data->data[i];
//            if (c >= u'a' && c <= u'z')
//                result.push_back(c - 32);       // A-Z
//            else
//                result.push_back(c);            // 其他字符原样
//        }
//        return String(result);
//    }
//
//    String String::ToLower() const {
//        if (!m_data) return String();
//        std::u16string result;
//        result.reserve(m_data->size);
//
//        for (size_t i = 0; i < m_data->size; ++i) {
//            char16_t c = m_data->data[i];
//            if (c >= u'A' && c <= u'Z')
//                result.push_back(c + 32);
//            else
//                result.push_back(c);
//        }
//        return String(result);
//    }
//
//    void String::ToUpperInPlace() {
//        if (!m_data) return;
//        Detach();  // COW 分离
//        for (size_t i = 0; i < m_data->size; ++i) {
//            char16_t c = m_data->data[i];
//            if (c >= u'a' && c <= u'z')
//                m_data->data[i] = c - 32;
//        }
//        m_data->hashValid = false;
//    }
//
//    void String::ToLowerInPlace() {
//        if (!m_data) return;
//        Detach();  // COW 分离
//        for (size_t i = 0; i < m_data->size; ++i) {
//            char16_t c = m_data->data[i];
//            if (c >= u'A' && c <= u'Z')
//                m_data->data[i] = c + 32;
//        }
//        m_data->hashValid = false;
//    }
//
//    size_t String::FindImpl(const String& str, size_t start, bool ignoreCase = false, bool reverse = false) const {
//        if (!m_data || str.Empty() || start >= m_data->size) return npos;
//
//        const char16_t* text = m_data->data;
//        const char16_t* pattern = str.m_data->data;
//        size_t n = m_data->size;
//        size_t m = str.m_data->size;
//
//        if (m > n) return npos;
//
//        if (!reverse) {
//            // --- 正向 BM 查找（保持原实现） ---
//            std::array<size_t, 65536> badShift{};
//            badShift.fill(m);
//            for (size_t i = 0; i < m - 1; ++i)
//                badShift[static_cast<uint16_t>(pattern[i])] = m - 1 - i;
//
//            std::vector<size_t> goodShift(m + 1, m);
//            std::vector<size_t> borderPos(m + 1);
//            size_t i = m, j = m + 1;
//            borderPos[i] = j;
//            while (i > 0) {
//                while (j <= m && pattern[i - 1] != pattern[j - 1]) {
//                    if (goodShift[j] == m) goodShift[j] = m - i;
//                    j = borderPos[j];
//                }
//                --i; --j;
//                borderPos[i] = j;
//            }
//            j = borderPos[0];
//            for (i = 0; i <= m; ++i) {
//                if (goodShift[i] == m) goodShift[i] = j;
//                if (i == j) j = borderPos[j];
//            }
//
//            size_t pos = start;
//            while (pos <= n - m) {
//                size_t k = 0;
//                while (k < m && pattern[k] == text[pos + k]) ++k;
//                if (k == m) return pos; // 命中
//                size_t bcShift = badShift[static_cast<uint16_t>(text[pos + k])] - (m - 1 - k);
//                size_t gsShift = goodShift[k + 1];
//                pos += std::max<size_t>(1, std::max(bcShift, gsShift));
//            }
//            return npos;
//        }
//
//        // --- 反向 BM 查找 ---
//        std::array<size_t, 65536> badShift{};
//        badShift.fill(m);
//        for (size_t i = 1; i < m; ++i)
//            badShift[static_cast<uint16_t>(pattern[i])] = i;
//
//        std::vector<size_t> goodShift(m + 1, m);
//        std::vector<size_t> borderPos(m + 1);
//        size_t i = m, j = m + 1;
//        borderPos[i] = j;
//        while (i > 0) {
//            while (j <= m && pattern[i - 1] != pattern[j - 1]) {
//                if (goodShift[j] == m) goodShift[j] = j - i;
//                j = borderPos[j];
//            }
//            --i; --j;
//            borderPos[i] = j;
//        }
//        j = borderPos[0];
//        for (i = 0; i <= m; ++i) {
//            if (goodShift[i] == m) goodShift[i] = j;
//            if (i == j) j = borderPos[j];
//        }
//
//        size_t pos = (start + 1 >= m) ? start + 1 - m : 0;
//        while (pos != npos) {
//            size_t k = m;
//            while (k > 0 && pattern[k - 1] == text[pos + k - 1]) --k;
//            if (k == 0) return pos; // 命中
//            size_t bcShift = badShift[static_cast<uint16_t>(text[pos + k - 1])];
//            size_t gsShift = goodShift[k];
//            if (pos < std::max(bcShift, gsShift)) break;
//            pos -= std::max(bcShift, gsShift);
//        }
//
//        return npos;
//    }
//
//    size_t String::FindIgnoreCase(const String& substr, size_t start) const {
//        String upperThis = this->ToUpper();
//        String upperSub = substr.ToUpper();
//        return upperThis.Find(upperSub, start);
//    }
//
//    std::u16string String::Utf8ToUtf16(const std::string& utf8) {
//        std::u16string result;
//        size_t i = 0;
//        while (i < utf8.size()) {
//            uint32_t codepoint = 0;
//            unsigned char c = static_cast<unsigned char>(utf8[i]);
//            size_t extraBytes = 0;
//
//            if (c <= 0x7F) {
//                codepoint = c;
//                extraBytes = 0;
//            }
//            else if ((c & 0xE0) == 0xC0) {
//                codepoint = c & 0x1F;
//                extraBytes = 1;
//            }
//            else if ((c & 0xF0) == 0xE0) {
//                codepoint = c & 0x0F;
//                extraBytes = 2;
//            }
//            else if ((c & 0xF8) == 0xF0) {
//                codepoint = c & 0x07;
//                extraBytes = 3;
//            }
//            else {
//                throw std::runtime_error("Invalid UTF-8 lead byte");
//            }
//
//            if (i + extraBytes >= utf8.size())
//                throw std::runtime_error("Unexpected end of UTF-8 string");
//
//            for (size_t j = 1; j <= extraBytes; ++j) {
//                unsigned char cont = static_cast<unsigned char>(utf8[i + j]);
//                if ((cont & 0xC0) != 0x80)
//                    throw std::runtime_error("Invalid UTF-8 continuation byte");
//                codepoint = (codepoint << 6) | (cont & 0x3F);
//            }
//
//            // UTF-16 encoding
//            if (codepoint <= 0xFFFF) {
//                result.push_back(static_cast<char16_t>(codepoint));
//            }
//            else if (codepoint <= 0x10FFFF) {
//                codepoint -= 0x10000;
//                result.push_back(static_cast<char16_t>((codepoint >> 10) + 0xD800));
//                result.push_back(static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00));
//            }
//            else {
//                throw std::runtime_error("Invalid Unicode codepoint");
//            }
//
//            i += extraBytes + 1;
//        }
//        return result;
//    }
//
//    std::u16string String::Utf32ToUtf16(const std::u32string& utf32) {
//        std::u16string result;
//        for (char32_t codepoint : utf32) {
//            if (codepoint <= 0xFFFF) {
//                result.push_back(static_cast<char16_t>(codepoint));
//            }
//            else if (codepoint <= 0x10FFFF) {
//                codepoint -= 0x10000;
//                result.push_back(static_cast<char16_t>((codepoint >> 10) + 0xD800));
//                result.push_back(static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00));
//            }
//            else {
//                throw std::runtime_error("Invalid UTF-32 codepoint");
//            }
//        }
//        return result;
//    }
//
//    std::u16string String::GbkToUtf16(const std::string& gbk) {
//        if (gbk.empty()) return std::u16string();
//#ifdef _WIN32
//        int sizeNeeded = MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), nullptr, 0);
//        if (sizeNeeded <= 0) throw std::runtime_error("GbkToUtf16 failed");
//        std::wstring wstr(sizeNeeded, 0);
//        MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), &wstr[0], sizeNeeded);
//        return std::u16string(wstr.begin(), wstr.end());
//#else
//        std::mbstate_t state{};
//        const char* src = gbk.data();
//        size_t len = std::mbsrtowcs(nullptr, &src, 0, &state);
//        if (len == static_cast<size_t>(-1)) throw std::runtime_error("GbkToUtf16 failed");
//        std::wstring wide(len, 0);
//        std::mbsrtowcs(&wide[0], &src, len, &state);
//        return std::u16string(wide.begin(), wide.end());
//#endif
//    }
//
//    std::string String::Utf16ToUtf8(const std::u16string& utf16) {
//        std::string result;
//        size_t i = 0;
//        while (i < utf16.size()) {
//            uint32_t codepoint = utf16[i];
//
//            // 判断是否为高代理项
//            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
//                if (i + 1 >= utf16.size())
//                    throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
//                char32_t low = utf16[i + 1];
//                if (low < 0xDC00 || low > 0xDFFF)
//                    throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
//                codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
//                i++;
//            }
//            else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
//                throw std::runtime_error("Invalid UTF-16 string: unexpected low surrogate");
//            }
//
//            // UTF-8 encoding
//            if (codepoint <= 0x7F) {
//                result.push_back(static_cast<char>(codepoint));
//            }
//            else if (codepoint <= 0x7FF) {
//                result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
//                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
//            }
//            else if (codepoint <= 0xFFFF) {
//                result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
//                result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
//                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
//            }
//            else if (codepoint <= 0x10FFFF) {
//                result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
//                result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
//                result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
//                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
//            }
//            else {
//                throw std::runtime_error("Invalid Unicode codepoint");
//            }
//
//            i++;
//        }
//        return result;
//    }
//
//    std::u32string String::Utf16ToUtf32(const std::u16string& utf16) {
//        std::u32string result;
//        size_t i = 0;
//        while (i < utf16.size()) {
//            char32_t codepoint = utf16[i];
//
//            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) { // 高代理项
//                if (i + 1 >= utf16.size())
//                    throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
//                char32_t low = utf16[i + 1];
//                if (low < 0xDC00 || low > 0xDFFF)
//                    throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
//                codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
//                i++;
//            }
//            else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
//                throw std::runtime_error("Invalid UTF-16 string: unexpected low surrogate");
//            }
//
//            result.push_back(codepoint);
//            i++;
//        }
//        return result;
//    }
//    
//    std::string String::Utf16ToGbk(const std::u16string& utf16) {
//        if (utf16.empty()) return std::string();
//#ifdef _WIN32
//        int sizeNeeded = WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
//        if (sizeNeeded <= 0) throw std::runtime_error("Utf16ToGbk failed");
//        std::string gbk(sizeNeeded, 0);
//        WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), &gbk[0], sizeNeeded, nullptr, nullptr);
//        return gbk;
//#else
//        std::mbstate_t state{};
//        const wchar_t* src = reinterpret_cast<const wchar_t*>(utf16.data());
//        size_t len = std::wcsrtombs(nullptr, &src, 0, &state);
//        if (len == static_cast<size_t>(-1)) throw std::runtime_error("Utf16ToGbk failed");
//        std::string gbk(len, '\0');
//        std::wcsrtombs(&gbk[0], &src, len, &state);
//        return gbk;
//#endif
//    }
//
//    void String::Detach() {
//        if (m_data && m_data->refCount.load() > 1) {
//            StringData* newData = new StringData(m_data->data, m_data->size);
//            m_data->refCount.fetch_sub(1);
//            m_data = newData;
//        }
//    }
//
//    void String::EnsureCapacity(size_t required) {
//        // required: 需要的字符（UTF-16 单元）容量
//        if (!m_data) return;
//
//        if (required <= m_data->capacity) return; // 容量够
//
//        // COW: 如果当前缓冲区是共享的，先复制一份
//        if (m_data->refCount.load() > 1) {
//            Detach();  // 生成独占副本
//            if (required <= m_data->capacity) return;
//        }
//
//        // 分配更大的缓冲区
//        size_t newCap = std::max<size_t>(required, (size_t)(m_data->capacity * (size_t)2));
//        char16_t* newBuf = new char16_t[newCap + 1]; // +1 给 '\0'
//
//        std::memcpy(newBuf, m_data->data, m_data->size * sizeof(char16_t));
//        newBuf[m_data->size] = u'\0';
//
//        delete[] m_data->data;
//        m_data->data = newBuf;
//        m_data->capacity = newCap;
//    }
//
//    struct String::StringData {
//        char16_t* data;               // UTF-16 数据
//        size_t size;              // 字节长度
//        size_t capacity;          // 内存容量
//        std::atomic<int> refCount; // 引用计数
//        Encoding encoding; // 编码方式
//
//        size_t lengthCache;   // 字符数缓存
//        bool lengthValid; // 字符数是否有效
//
//        size_t hashCache;  // 哈希缓存
//        bool hashValid;    // 是否有效
//
//        StringData(const char16_t* s, size_t len) : size(len), capacity(len), refCount(1) {
//            data = new char16_t[len + 1];
//            std::memcpy(data, s, len * sizeof(char16_t));
//            data[len] = '\0';
//        }
//        ~StringData() { delete[] data; }
//    };
//}