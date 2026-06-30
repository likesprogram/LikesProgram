#include <unicode/Convert.hpp>
#include <stdexcept>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#include <cerrno>
#endif
namespace LikesProgram {
    namespace Unicode {
        namespace Convert {
            // 严格 UTF-8 -> UTF-16 转换，拒绝 overlong、surrogate 和截断序列。
            std::u16string Utf8ToUtf16(const std::u8string& utf8) {
#ifdef _WIN32
                if (utf8.empty()) return std::u16string();
                if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    throw std::length_error("UTF-8 input too large");
                }

                const auto* bytes = reinterpret_cast<const char*>(utf8.data()); // Windows API 需要 char 字节指针
                int inputSize = static_cast<int>(utf8.size());                  // 输入 UTF-8 字节数
                int sizeNeeded = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, inputSize, nullptr, 0); // 目标 UTF-16 code unit 数
                if (sizeNeeded <= 0) throw std::runtime_error("Invalid UTF-8 string");

                std::u16string result(static_cast<size_t>(sizeNeeded), u'\0'); // 按精确长度预分配 UTF-16 输出
                int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, inputSize, // 实际写入 code unit 数
                    reinterpret_cast<wchar_t*>(result.data()), sizeNeeded);
                if (written != sizeNeeded) throw std::runtime_error("Utf8ToUtf16 failed");
                return result;
#else
                std::u16string result; // UTF-16 输出缓冲
                result.reserve(utf8.size());
                size_t i = 0; // 当前 UTF-8 字节偏移
                while (i < utf8.size()) {
                    uint32_t codepoint = 0; // 当前解码得到的 Unicode code point
                    unsigned char c = static_cast<unsigned char>(utf8[i]); // 当前 UTF-8 lead byte
                    size_t extraBytes = 0;    // 当前序列需要的 continuation byte 数
                    uint32_t minCodepoint = 0; // 用于 overlong 检测的最小合法值

                    if (c <= 0x7F) {
                        codepoint = c;
                        extraBytes = 0;
                        minCodepoint = 0;
                    }
                    else if ((c & 0xE0) == 0xC0) {
                        codepoint = c & 0x1F;
                        extraBytes = 1;
                        minCodepoint = 0x80;
                    }
                    else if ((c & 0xF0) == 0xE0) {
                        codepoint = c & 0x0F;
                        extraBytes = 2;
                        minCodepoint = 0x800;
                    }
                    else if ((c & 0xF8) == 0xF0) {
                        codepoint = c & 0x07;
                        extraBytes = 3;
                        minCodepoint = 0x10000;
                    }
                    else {
                        throw std::runtime_error("Invalid UTF-8 lead byte");
                    }

                    if (i + extraBytes >= utf8.size())
                        throw std::runtime_error("Unexpected end of UTF-8 string");

                    for (size_t j = 1; j <= extraBytes; ++j) {
                        unsigned char cont = static_cast<unsigned char>(utf8[i + j]);
                        if ((cont & 0xC0) != 0x80)
                            throw std::runtime_error("Invalid UTF-8 continuation byte");
                        codepoint = (codepoint << 6) | (cont & 0x3F);
                    }

                    if (codepoint < minCodepoint)
                        throw std::runtime_error("Invalid UTF-8 overlong sequence");
                    // UTF-8 不能编码代理区，也不能超过 Unicode 最大码点。
                    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
                        throw std::runtime_error("Invalid UTF-8 surrogate codepoint");
                    // 超过 Unicode 上限的序列即使字节形态合法也必须拒绝。
                    if (codepoint > 0x10FFFF)
                        throw std::runtime_error("Invalid UTF-8 codepoint");

                    // UTF-16 encoding
                    if (codepoint <= 0xFFFF) {
                        result.push_back(static_cast<char16_t>(codepoint));
                    }
                    else if (codepoint <= 0x10FFFF) {
                        codepoint -= 0x10000;
                        result.push_back(static_cast<char16_t>((codepoint >> 10) + 0xD800));
                        result.push_back(static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00));
                    }
                    else {
                        throw std::runtime_error("Invalid Unicode codepoint");
                    }

                    i += extraBytes + 1;
                }
                return result;
#endif
            }

            // 严格 UTF-32 -> UTF-16 转换，拒绝 surrogate 和超范围 code point。
            std::u16string Utf32ToUtf16(const std::u32string& utf32) {
                std::u16string result; // UTF-16 输出缓冲
                result.reserve(utf32.size());
                for (char32_t codepoint : utf32) {
                    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                        throw std::runtime_error("Invalid UTF-32 surrogate codepoint");
                    }
                    if (codepoint <= 0xFFFF) {
                        result.push_back(static_cast<char16_t>(codepoint));
                    }
                    else if (codepoint <= 0x10FFFF) {
                        codepoint -= 0x10000;
                        result.push_back(static_cast<char16_t>((codepoint >> 10) + 0xD800));
                        result.push_back(static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00));
                    }
                    else {
                        throw std::runtime_error("Invalid UTF-32 codepoint");
                    }
                }
                return result;
            }

            // GBK -> UTF-16 转换，平台优先使用系统编码 API。
            std::u16string GbkToUtf16(const std::string& gbk) {
                if (gbk.empty()) return std::u16string();
#ifdef _WIN32
                int sizeNeeded = MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), nullptr, 0); // 目标宽字符数
                if (sizeNeeded <= 0) throw std::runtime_error("GbkToUtf16 failed");
                std::wstring wstr(sizeNeeded, 0); // Windows 宽字符中间缓冲
                MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), &wstr[0], sizeNeeded);
                return std::u16string(wstr.begin(), wstr.end());
#else
                iconv_t cd = iconv_open("UTF-16LE", "GBK"); // POSIX 编码转换句柄
                if (cd == (iconv_t)-1)
                    throw std::runtime_error("GbkToUtf16 failed: iconv_open");

                const char* inBuf = gbk.data(); // iconv 输入游标
                size_t inBytes = gbk.size();    // 剩余输入字节数

                size_t outBytes = gbk.size() * 4 + 8; // UTF-16 最多 4B/字
                std::string out(outBytes, '\0'); // iconv 输出字节缓冲
                char* outPtr = out.data();       // iconv 输出游标

                size_t res = iconv(cd, // iconv 转换状态码
                    const_cast<char**>(&inBuf), &inBytes,
                    &outPtr, &outBytes);
                if (res == (size_t)-1) {
                    iconv_close(cd);
                    throw std::runtime_error("GbkToUtf16 failed: iconv (" +
                        std::to_string(errno) + ")");
                }
                iconv_close(cd);

                size_t utf16Bytes = out.size() - outBytes; // 实际写入的 UTF-16 字节数
                return std::u16string(reinterpret_cast<const char16_t*>(out.data()),
                    utf16Bytes / sizeof(char16_t));
#endif
            }

            // 严格 UTF-16 -> UTF-8 转换，拒绝孤立代理项。
            std::u8string Utf16ToUtf8(const std::u16string& utf16) {
#ifdef _WIN32
                if (utf16.empty()) return std::u8string();
                if (utf16.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    throw std::length_error("UTF-16 input too large");
                }

                const auto* wide = reinterpret_cast<const wchar_t*>(utf16.data()); // Windows wchar_t 与 UTF-16 同宽
                int inputSize = static_cast<int>(utf16.size());                    // 输入 UTF-16 code unit 数
                int sizeNeeded = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, inputSize, nullptr, 0, nullptr, nullptr); // 目标 UTF-8 字节数
                if (sizeNeeded <= 0) throw std::runtime_error("Invalid UTF-16 string");

                std::u8string result(static_cast<size_t>(sizeNeeded), u8'\0'); // 按精确字节数预分配输出
                int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, inputSize, // 实际写入字节数
                    reinterpret_cast<char*>(result.data()), sizeNeeded, nullptr, nullptr);
                if (written != sizeNeeded) throw std::runtime_error("Utf16ToUtf8 failed");
                return result;
#else
                std::u8string result; // UTF-8 输出缓冲
                result.reserve(utf16.size() * 3);
                size_t i = 0; // 当前 UTF-16 code unit 偏移
                while (i < utf16.size()) {
                    uint32_t codepoint = utf16[i]; // 当前解码的 code point 初值

                    // 判断是否为高代理项
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (i + 1 >= utf16.size())
                            throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
                        char32_t low = utf16[i + 1]; // 当前高代理之后的低代理候选
                        if (low < 0xDC00 || low > 0xDFFF)
                            throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
                        codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
                        // 已消费低代理，循环尾部再前进一个单元。
                        i++;
                    }
                    else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        throw std::runtime_error("Invalid UTF-16 string: unexpected low surrogate");
                    }

                    // UTF-8 encoding
                    if (codepoint <= 0x7F) {
                        result.push_back(static_cast<char>(codepoint));
                    }
                    else if (codepoint <= 0x7FF) {
                        result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    else if (codepoint <= 0xFFFF) {
                        result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    else if (codepoint <= 0x10FFFF) {
                        result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
                        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    else {
                        throw std::runtime_error("Invalid Unicode codepoint");
                    }

                    i++;
                }
                return result;
#endif
            }

            std::u32string Utf16ToUtf32(const std::u16string& utf16) {
                std::u32string result; // 转换后的 UTF-32 输出缓冲
                result.reserve(utf16.size());
                size_t i = 0; // 当前 UTF-16 code unit 偏移
                while (i < utf16.size()) {
                    char32_t codepoint = utf16[i]; // 当前解码的 code point 初值

                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) { // 高代理项
                        if (i + 1 >= utf16.size())
                            throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
                        char32_t low = utf16[i + 1]; // 当前高代理之后的低代理候选
                        if (low < 0xDC00 || low > 0xDFFF)
                            throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
                        codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
                        // 已消费低代理，循环尾部再前进一个单元。
                        i++;
                    }
                    else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        throw std::runtime_error("Invalid UTF-16 string: unexpected low surrogate");
                    }

                    result.push_back(codepoint);
                    i++;
                }
                return result;
            }

            std::string Utf16ToGbk(const std::u16string& utf16) {
                if (utf16.empty()) return std::string();
#ifdef _WIN32
                int sizeNeeded = WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr); // 目标 GBK 字节数
                if (sizeNeeded <= 0) throw std::runtime_error("Utf16ToGbk failed");
                std::string gbk(sizeNeeded, 0); // Windows API 输出 GBK 字节缓冲
                WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), &gbk[0], sizeNeeded, nullptr, nullptr);
                return gbk;
#else
                iconv_t cd = iconv_open("GBK//TRANSLIT", "UTF-16LE"); // POSIX 编码转换句柄
                if (cd == (iconv_t)-1)
                    throw std::runtime_error("Utf16ToGbk failed: iconv_open");

                const char* inBuf = reinterpret_cast<const char*>(utf16.data()); // iconv 输入游标
                size_t inBytes = utf16.size() * sizeof(char16_t); // 剩余 UTF-16 输入字节数

                // 估算输出缓冲区：GBK 最大 2B/字符，再多留一点
                size_t outBytes = utf16.size() * 3 + 8; // iconv 输出缓冲剩余字节数
                std::string out(outBytes, '\0'); // iconv 输出字节缓冲
                char* outPtr = out.data(); // iconv 输出游标

                size_t res = iconv(cd, // iconv 转换状态码
                    const_cast<char**>(&inBuf), &inBytes,
                    &outPtr, &outBytes);

                if (res == (size_t)-1) {
                    iconv_close(cd);
                    throw std::runtime_error("Utf16ToGbk failed: iconv error (" +
                        std::to_string(errno) + ")");
                }
                iconv_close(cd);
                out.resize(out.size() - outBytes);   // 截取实际长度
                return out;
#endif
            }
        }
    }
}
