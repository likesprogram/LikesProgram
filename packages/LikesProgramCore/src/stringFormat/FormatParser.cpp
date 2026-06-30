#include <stringFormat/FormatParser.hpp>

#include <limits>
#include <sstream>
#include <stdexcept>

namespace LikesProgram {
    namespace StringFormat {
        namespace {
            // 只接受 ASCII 数字，避免 Unicode 数字参与格式语法。
            bool IsAsciiDigit(char32_t c) {
                return c >= U'0' && c <= U'9';
            }
        }

        // 主解析器把格式串拆成 literal 与 placeholder，错误以 Result 聚合返回。
        FormatParser::Result FormatParser::Parse(const String& fmt) {
            Result res; // 当前解析结果
            size_t pos = 0; // 当前 code point 位置
            String literal; // 尚未提交的普通文本片段
            const size_t fmtSize = fmt.Size(); // 格式串 code point 数

            while (pos < fmtSize) {
                const char32_t c = fmt[pos]; // 当前格式字符

                if (c == U'{') {
                    if (pos + 1 < fmtSize && fmt[pos + 1] == U'{') {
                        literal.Append(U'{');
                        pos += 2;
                        continue;
                    }

                    if (!literal.Empty()) {
                        Token t; // 普通文本 token
                        t.isPlaceholder = false;
                        t.literal = literal;
                        t.position = pos - literal.Size();
                        // 普通文本 token 在进入占位符前必须先提交。
                        res.tokens.push_back(std::move(t));
                        // 普通文本已提交，后续从占位符重新累计 literal。
                        // 这里清空而不重新分配，保留 String 内部容量。
                        literal.Clear();
                    }

                    const size_t tokenStart = pos; // 当前占位符起始位置
                    ++pos;
                    const size_t contentStart = pos; // 占位符内容起始位置
                    while (pos < fmtSize && fmt[pos] != U'}') {
                        if (fmt[pos] == U'{') {
                            res.hasFatalError = true;
                            res.errors.push_back({ tokenStart, String("nested or unmatched '{'") });
                            return res;
                        }
                        ++pos;
                    }

                    if (pos >= fmtSize) {
                        res.hasFatalError = true;
                        res.errors.push_back({ tokenStart, String("missing '}'") });
                        return res;
                    }

                    Token t; // 占位符 token
                    t.isPlaceholder = true;
                    t.position = tokenStart;
                    String content = fmt.SubString(contentStart, pos - contentStart); // 去掉大括号后的占位符内容
                    ++pos;

                    try {
                        FormatSpec spec = ParsePlaceholder(content);
                        spec.SetRaw(content);
                        t.spec = std::move(spec);
                    }
                    catch (const std::exception& ex) {
                        res.errors.push_back({ tokenStart, String(std::string("ParsePlaceholder error: ") + ex.what()) });
                        t.isPlaceholder = false;
                        t.literal = String(U"{!}");
                    }
                    catch (...) {
                        res.errors.push_back({ tokenStart, String("ParsePlaceholder unknown error") });
                        t.isPlaceholder = false;
                        t.literal = String(U"{!}");
                    }

                    res.tokens.push_back(std::move(t));
                    continue;
                }

                if (c == U'}') {
                    if (pos + 1 < fmtSize && fmt[pos + 1] == U'}') {
                        literal.Append(U'}');
                        pos += 2;
                        continue;
                    }

                    res.hasFatalError = true;
                    res.errors.push_back({ pos, String("unmatched '}'") });
                    return res;
                }

                literal.Append(c);
                ++pos;
            }

            if (!literal.Empty()) {
                Token t; // 收尾普通文本 token
                t.isPlaceholder = false;
                t.literal = literal;
                t.position = fmtSize - literal.Size();
                // 文件尾部的普通文本也需要提交为 token。
                res.tokens.push_back(std::move(t));
            }

            return res;
        }

        // 快速校验大括号结构，供外部在不需要完整解析时使用。
        bool FormatParser::ValidateBraces(const String& fmt) {
            size_t pos = 0; // 当前 code point 位置
            const size_t fmtSize = fmt.Size(); // 格式串 code point 数
            while (pos < fmtSize) {
                const char32_t c = fmt[pos]; // 当前格式字符
                if (c == U'{') {
                    if (pos + 1 < fmtSize && fmt[pos + 1] == U'{') {
                        pos += 2;
                        continue;
                    }
                    ++pos;
                    while (pos < fmtSize && fmt[pos] != U'}') {
                        if (fmt[pos] == U'{') return false;
                        ++pos;
                    }
                    if (pos >= fmtSize) return false;
                    ++pos;
                    continue;
                }
                if (c == U'}') {
                    if (pos + 1 < fmtSize && fmt[pos + 1] == U'}') {
                        pos += 2;
                        continue;
                    }
                    return false;
                }
                ++pos;
            }
            return true;
        }

        // 从指定位置提取下一个 token，复用完整解析逻辑保持行为一致。
        bool FormatParser::ExtractNextToken(const String& fmt, size_t& pos, String& outContent, bool& outIsPlaceholder) {
            const size_t fmtSize = fmt.Size(); // 格式串 code point 数
            Result res = FormatParser().Parse(fmt.SubString(pos, fmtSize - pos)); // 从当前位置开始的解析结果
            if (res.tokens.empty()) return false;
            const Token& t = res.tokens.front(); // 下一个可返回 token
            outIsPlaceholder = t.isPlaceholder;
            outContent = t.isPlaceholder ? t.spec.GetRaw() : t.literal;
            pos += outContent.Size() + (t.isPlaceholder ? 2 : 0);
            // 占位符额外跨过左右大括号，普通文本只跨过自身长度。
            return true;
        }

        // 解析单个占位符内部内容，顺序为 index、冒号选项、尾部校验。
        FormatSpec FormatParser::ParsePlaceholder(const String& inside) {
            FormatSpec spec; // 输出格式规格
            size_t i = 0; // 当前 code point 位置
            const size_t insideSize = inside.Size(); // 占位符内部 code point 数

            SkipSpaces(inside, i);
            auto idx = ParseNumber(inside, i); // 可选显式参数索引
            if (idx) spec.SetIndex(*idx, true);

            SkipSpaces(inside, i);
            if (i < insideSize && inside[i] == U':') {
                ++i;
                ParseFormatOptions(inside, i, spec);
            }

            SkipSpaces(inside, i);
            if (i != insideSize) {
                ThrowFormatError(U"invalid placeholder tail", inside, i);
            }

            return spec;
        }

        void FormatParser::SkipSpaces(const String& s, size_t& i) {
            const size_t sSize = s.Size(); // 输入 code point 数
            while (i < sSize && (s[i] == U' ' || s[i] == U'\t')) ++i;
        }

        std::optional<int> FormatParser::ParseNumber(const String& s, size_t& i) {
            const size_t sSize = s.Size(); // 输入 code point 数
            if (i >= sSize || !IsAsciiDigit(s[i])) return std::nullopt;

            int val = 0; // 当前解析出的非负整数
            while (i < sSize && IsAsciiDigit(s[i])) {
                const int digit = static_cast<int>(s[i] - U'0'); // 当前 ASCII 数字值
                if (val > (std::numeric_limits<int>::max() - digit) / 10) {
                    ThrowFormatError(U"number too large", s, i);
                }
                val = val * 10 + digit;
                ++i;
            }
            return val;
        }

        String FormatParser::ParseFillChar(const String& s, size_t& i) {
            const size_t sSize = s.Size();
            if (i >= sSize) return String();

            if (s[i] == U'\'' || s[i] == U'"') {
                const char32_t quote = s[i++]; // 引号包裹填充符的起始引号
                String fill; // 解析出的填充符内容
                bool closed = false; // 引号是否正常闭合

                while (i < sSize) {
                    char32_t c = s[i++]; // 当前解析到的填充符 code point
                    if (c == quote) {
                        closed = true;
                        break;
                    }
                    if (c == U'\\' && i < sSize) {
                        c = s[i++];
                        switch (c) {
                        case U'n': fill.Append(U'\n'); break;
                        case U't': fill.Append(U'\t'); break;
                        case U'r': fill.Append(U'\r'); break;
                        case U'\\': fill.Append(U'\\'); break;
                        case U'\'': fill.Append(U'\''); break;
                        case U'"': fill.Append(U'"'); break;
                        default: fill.Append(c); break;
                        }
                    }
                    else {
                        fill.Append(c);
                    }
                }

                if (!closed || i >= sSize || !IsAlignChar(s[i])) {
                    ThrowFormatError(U"quoted fill must be followed by alignment", s, i);
                }
                return fill;
            }

            if (i + 1 < sSize && IsAlignChar(s[i + 1])) {
                String fill(s[i]);
                ++i;
                return fill;
            }

            return String();
        }

        bool FormatParser::IsAlignChar(char32_t c) {
            // 对齐字符与 Python/format 风格保持一致。
            return c == U'<' || c == U'>' || c == U'^' || c == U'=';
        }

        void FormatParser::ParseFormatOptions(const String& s, size_t& i, FormatSpec& spec) {
            const size_t sSize = s.Size();
            String fill = ParseFillChar(s, i);
            if (!fill.Empty()) spec.SetFill(fill);

            if (i < sSize && IsAlignChar(s[i])) {
                spec.SetAlign(s[i]);
                ++i;
            }

            if (i < sSize && (s[i] == U'+' || s[i] == U'-' || s[i] == U' ')) {
                spec.SetSign(s[i]);
                ++i;
            }

            if (i < sSize && s[i] == U'#') {
                spec.SetAlternateForm(true);
                ++i;
            }

            if (i < sSize && s[i] == U'0') {
                spec.SetZeroPad(true);
                spec.SetFill(U"0");
                spec.SetAlign(U'=');
                ++i;
            }

            if (auto w = ParseNumber(s, i)) spec.SetWidth(*w);

            if (i < sSize && s[i] == U'.') {
                ++i;
                if (auto p = ParseNumber(s, i)) spec.SetPrecision(*p);
                else ThrowFormatError(U"invalid precision", s, i);
            }

            if (i < sSize) {
                spec.SetType(s[i]);
                ++i;
                if (i < sSize) {
                    spec.SetTypeExpand(s.SubString(i, sSize - i));
                    i = sSize;
                }
            }
        }

        bool FormatParser::IsTypeChar(char32_t c) {
            static const char32_t types[] = { // 当前格式解析器支持的类型字符表
                U's', U'S', U'd', U'i', U'o', U'O', U'u', U'x', U'X',
                U'b', U'B', U'f', U'F', U'e', U'E', U'g', U'G',
                U'c', U'p', U'P', U't', U'T', U'%'
            };
            for (auto t : types) {
                if (c == t) return true;
            }
            return false;
        }

        [[noreturn]] void FormatParser::ThrowFormatError(const String& msg, const String& context, size_t pos) {
            std::stringstream ss; // 组装异常消息的字节流
            ss << "[FormatParserError] " << msg.ToStdString()
                << " pos=" << pos
                << " context=\"" << context.ToStdString() << "\"";
            throw std::runtime_error(ss.str());
        }
    }
}
