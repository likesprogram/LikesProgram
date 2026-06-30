#include <LikesProgram/Core/time/Time.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace LikesProgram {
    namespace Time {
        LikesProgram::String FormatTime(const TimePoint& tp, const LikesProgram::String& fmt)
        {
            std::time_t t = std::chrono::system_clock::to_time_t(tp); // 秒级本地时间转换输入
            std::tm tm{}; // 平台 localtime 输出结构
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif

            // 纳秒部分
            auto ns = duration_cast<Nanoseconds>(tp.time_since_epoch()) % Seconds(1); // 当前秒内的纳秒余数
            int64_t ns_val = ns.count(); // 归一化前纳秒余数
            if (ns_val < 0) ns_val += 1'000'000'000;
            int ms_val = static_cast<int>(ns_val / 1000000); // SSS 扩展使用的毫秒值

            std::wstring result; // 格式化后的宽字符输出
            result.reserve(128);
            wchar_t buf[128]; // strftime/swprintf 临时缓冲

            static const wchar_t* weekNames[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" }; // WW 扩展星期缩写

            const size_t fmtSize = fmt.Size(); // 格式串 code point 数
            for (size_t i = 0; i < fmtSize; ++i) {
                // 自定义扩展优先于 strftime，以支持 SSS、AP/ap、WW 等旧风格 token。
                if (i + 2 < fmtSize && fmt.At(i) == u'S' && fmt.At(i + 1) == u'S' && fmt.At(i + 2) == u'S') {
                    swprintf(buf, 128, L"%03d", ms_val);
                    result += buf;
                    i += 2;
                    continue;
                }
                else if (i + 1 < fmtSize && ((fmt.At(i) == u'A' && fmt.At(i + 1) == u'P') || (fmt.At(i) == u'a' && fmt.At(i + 1) == u'p'))) {
                    bool lower = fmt.At(i) == u'a'; // ap 输出小写，AP 输出大写
                    result += (tm.tm_hour < 12 ? (lower ? L"am" : L"AM") : (lower ? L"pm" : L"PM"));
                    i += 1;
                    continue;
                }
                else if (fmt.At(i) == u'W') {
                    if (i + 1 < fmtSize && fmt.At(i + 1) == u'W') { // WW -> 星期英文缩写
                        result += weekNames[tm.tm_wday];
                        i += 1;
                    }
                    else { // W -> 数字 1~7
                        int w = (tm.tm_wday == 0 ? 7 : tm.tm_wday); // ISO 风格星期序号
                        swprintf(buf, 128, L"%d", w);
                        result += buf;
                    }
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'T' && fmt.At(i + 1) == u'Z') {
                    wcsftime(buf, 128, L"%z", &tm);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (i + 3 < fmtSize && fmt.At(i) == u'Y' && fmt.At(i + 1) == u'Y' && fmt.At(i + 2) == u'Y' && fmt.At(i + 3) == u'Y') {
                    swprintf(buf, 128, L"%04d", tm.tm_year + 1900);
                    result += buf;
                    i += 3;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'Y' && fmt.At(i + 1) == u'Y') {
                    swprintf(buf, 128, L"%02d", (tm.tm_year + 1900) % 100);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'M' && fmt.At(i + 1) == u'M') {
                    swprintf(buf, 128, L"%02d", tm.tm_mon + 1);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (fmt.At(i) == u'M') {
                    swprintf(buf, 128, L"%d", tm.tm_mon + 1);
                    result += buf;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'D' && fmt.At(i + 1) == u'D') {
                    swprintf(buf, 128, L"%02d", tm.tm_mday);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (fmt.At(i) == u'D') {
                    swprintf(buf, 128, L"%d", tm.tm_mday);
                    result += buf;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'h' && fmt.At(i + 1) == u'h') {
                    swprintf(buf, 128, L"%02d", tm.tm_hour);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u'm' && fmt.At(i + 1) == u'm') {
                    swprintf(buf, 128, L"%02d", tm.tm_min);
                    result += buf;
                    i += 1;
                    continue;
                }
                else if (i + 1 < fmtSize && fmt.At(i) == u's' && fmt.At(i + 1) == u's') {
                    swprintf(buf, 128, L"%02d", tm.tm_sec);
                    result += buf;
                    i += 1;
                    continue;
                }
                if (fmt.At(i) == u'%') {
                    if (i + 1 >= fmtSize) break;
                    wchar_t code = (wchar_t)fmt.At(i + 1); // strftime 兼容格式字符

                    // ==== 纳秒扩展 ====
                    if (code == L'f') {
                        // 默认 9 位
                        std::wostringstream oss; // 纳秒文本输出流
                        oss << std::setw(9) << std::setfill(L'0') << ns_val;
                        result += oss.str();
                        ++i;
                        continue;
                    }
                    else if (iswdigit(code) && i + 2 < fmtSize && fmt.At(i + 2) == u'f') {
                        int width = code - L'0'; // 支持 %3f / %6f / %9f
                        if (width > 9) width = 9;
                        std::wostringstream oss; // 指定位数纳秒文本输出流
                        oss << std::setw(width) << std::setfill(L'0') << (ns_val / static_cast<int>(std::pow(10, 9 - width)));
                        result += oss.str();
                        i += 2;
                        // 已消费形如 %3f 的完整扩展 token。
                        continue;
                    }

                    buf[0] = L'\0';
                    switch (code) {
                        // 日期
                    case L'a': wcsftime(buf, 128, L"%a", &tm); break;
                    case L'A': wcsftime(buf, 128, L"%A", &tm); break;
                    case L'b': wcsftime(buf, 128, L"%b", &tm); break;
                    case L'B': wcsftime(buf, 128, L"%B", &tm); break;
                    case L'c': wcsftime(buf, 128, L"%c", &tm); break;
                    case L'C': swprintf(buf, 128, L"%02d", (tm.tm_year + 1900) / 100); break;
                    case L'd': swprintf(buf, 128, L"%02d", tm.tm_mday); break;
                    case L'D': wcsftime(buf, 128, L"%D", &tm); break;
                    case L'e': swprintf(buf, 128, L"%2d", tm.tm_mday); break;
                    case L'F': wcsftime(buf, 128, L"%F", &tm); break;
                    case L'g': wcsftime(buf, 128, L"%g", &tm); break;
                    case L'G': wcsftime(buf, 128, L"%G", &tm); break;
                    case L'h': wcsftime(buf, 128, L"%b", &tm); break;
                    case L'H': swprintf(buf, 128, L"%02d", tm.tm_hour); break;
                    case L'I': swprintf(buf, 128, L"%02d", (tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12)); break;
                    case L'j': swprintf(buf, 128, L"%03d", tm.tm_yday + 1); break;
                    case L'm': swprintf(buf, 128, L"%02d", tm.tm_mon + 1); break;
                    case L'M': swprintf(buf, 128, L"%02d", tm.tm_min); break;
                    case L'n': result.push_back(L'\n'); ++i; continue;
                    case L'p': wcsftime(buf, 128, L"%p", &tm); break;
                    case L'r': wcsftime(buf, 128, L"%r", &tm); break;
                    case L'R': wcsftime(buf, 128, L"%R", &tm); break;
                    case L'S': swprintf(buf, 128, L"%02d", tm.tm_sec); break;
                    case L't': result.push_back(L'\t'); ++i; continue;
                    case L'T': wcsftime(buf, 128, L"%T", &tm); break;
                    case L'u': swprintf(buf, 128, L"%d", (tm.tm_wday == 0 ? 7 : tm.tm_wday)); break;
                    case L'U': wcsftime(buf, 128, L"%U", &tm); break;
                    case L'V': wcsftime(buf, 128, L"%V", &tm); break;
                    case L'w': swprintf(buf, 128, L"%d", tm.tm_wday); break;
                    case L'W': wcsftime(buf, 128, L"%W", &tm); break;
                    case L'x': wcsftime(buf, 128, L"%x", &tm); break;
                    case L'X': wcsftime(buf, 128, L"%X", &tm); break;
                    case L'y': swprintf(buf, 128, L"%02d", (tm.tm_year + 1900) % 100); break;
                    case L'Y': swprintf(buf, 128, L"%04d", tm.tm_year + 1900); break;
                    case L'z': wcsftime(buf, 128, L"%z", &tm); break;
                    case L'Z': wcsftime(buf, 128, L"%Z", &tm); break;
                    case L'%': result.push_back(L'%'); ++i; continue;
                    default:
                        result.push_back(L'%');
                        result.push_back(code);
                        ++i;
                        continue;
                    }

                    result += buf;
                    ++i;
                }
                else {
                    result.push_back((wchar_t)fmt.At(i));
                }
            }

            return LikesProgram::String(result);
        }

        std::tm ToLocalTime(std::time_t t) {
            std::tm tm{}; // 平台 localtime 输出结构
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            return tm;
        }
    }
}
