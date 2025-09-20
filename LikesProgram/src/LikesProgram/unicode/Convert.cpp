#include "../../../include/LikesProgram/unicode/Convert.hpp"
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#include <cerrno>
#endif
namespace LikesProgram {
    namespace Unicode {
        namespace Convert {
            std::u16string Utf8ToUtf16(const std::u8string& utf8) {
                std::u16string result;
                size_t i = 0;
                while (i < utf8.size()) {
                    uint32_t codepoint = 0;
                    unsigned char c = static_cast<unsigned char>(utf8[i]);
                    size_t extraBytes = 0;

                    if (c <= 0x7F) {
                        codepoint = c;
                        extraBytes = 0;
                    }
                    else if ((c & 0xE0) == 0xC0) {
                        codepoint = c & 0x1F;
                        extraBytes = 1;
                    }
                    else if ((c & 0xF0) == 0xE0) {
                        codepoint = c & 0x0F;
                        extraBytes = 2;
                    }
                    else if ((c & 0xF8) == 0xF0) {
                        codepoint = c & 0x07;
                        extraBytes = 3;
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
            }

            std::u16string Utf32ToUtf16(const std::u32string& utf32) {
                std::u16string result;
                for (char32_t codepoint : utf32) {
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

            std::u16string GbkToUtf16(const std::string& gbk) {
                if (gbk.empty()) return std::u16string();
#ifdef _WIN32
                int sizeNeeded = MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), nullptr, 0);
                if (sizeNeeded <= 0) throw std::runtime_error("GbkToUtf16 failed");
                std::wstring wstr(sizeNeeded, 0);
                MultiByteToWideChar(936, MB_PRECOMPOSED, gbk.data(), static_cast<int>(gbk.size()), &wstr[0], sizeNeeded);
                return std::u16string(wstr.begin(), wstr.end());
#else
                iconv_t cd = iconv_open("UTF-16LE", "GBK");
                if (cd == (iconv_t)-1)
                    throw std::runtime_error("GbkToUtf16 failed: iconv_open");

                const char* inBuf = gbk.data();
                size_t inBytes = gbk.size();

                size_t outBytes = gbk.size() * 4 + 8; // UTF-16 最多 4B/字
                std::string out(outBytes, '\0');
                char* outPtr = out.data();

                size_t res = iconv(cd,
                    const_cast<char**>(&inBuf), &inBytes,
                    &outPtr, &outBytes);
                if (res == (size_t)-1) {
                    iconv_close(cd);
                    throw std::runtime_error("GbkToUtf16 failed: iconv (" +
                        std::to_string(errno) + ")");
                }
                iconv_close(cd);

                size_t utf16Bytes = out.size() - outBytes;
                return std::u16string(reinterpret_cast<const char16_t*>(out.data()),
                    utf16Bytes / sizeof(char16_t));
#endif
            }

            std::u8string Utf16ToUtf8(const std::u16string& utf16) {
                std::u8string result;
                size_t i = 0;
                while (i < utf16.size()) {
                    uint32_t codepoint = utf16[i];

                    // 判断是否为高代理项
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (i + 1 >= utf16.size())
                            throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
                        char32_t low = utf16[i + 1];
                        if (low < 0xDC00 || low > 0xDFFF)
                            throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
                        codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
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
            }

            std::u32string Utf16ToUtf32(const std::u16string& utf16) {
                std::u32string result;
                size_t i = 0;
                while (i < utf16.size()) {
                    char32_t codepoint = utf16[i];

                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) { // 高代理项
                        if (i + 1 >= utf16.size())
                            throw std::runtime_error("Invalid UTF-16 string: dangling high surrogate");
                        char32_t low = utf16[i + 1];
                        if (low < 0xDC00 || low > 0xDFFF)
                            throw std::runtime_error("Invalid UTF-16 string: expected low surrogate");
                        codepoint = ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
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
                int sizeNeeded = WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
                if (sizeNeeded <= 0) throw std::runtime_error("Utf16ToGbk failed");
                std::string gbk(sizeNeeded, 0);
                WideCharToMultiByte(936, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), &gbk[0], sizeNeeded, nullptr, nullptr);
                return gbk;
#else
                iconv_t cd = iconv_open("GBK//TRANSLIT", "UTF-16LE");
                if (cd == (iconv_t)-1)
                    throw std::runtime_error("Utf16ToGbk failed: iconv_open");

                const char* inBuf = reinterpret_cast<const char*>(utf16.data());
                size_t inBytes = utf16.size() * sizeof(char16_t);

                // 估算输出缓冲区：GBK 最大 2B/字符，再多留一点
                size_t outBytes = utf16.size() * 3 + 8;
                std::string out(outBytes, '\0');
                char* outPtr = out.data();

                size_t res = iconv(cd,
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
