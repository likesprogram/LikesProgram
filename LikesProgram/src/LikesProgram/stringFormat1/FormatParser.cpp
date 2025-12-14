#include "../../../include/LikesProgram/StringFormat/FormatParser.hpp"
#include <stdexcept>
#include <cassert>
#include <sstream>
#include <set>
#include <iostream>

namespace LikesProgram {
	namespace StringFormat {
        // 主解析函数
        FormatParser::Result FormatParser::Parse(const String& fmt) {
            Result res;

            // 先做结构检查
            try {
                if (!ValidateBraces(fmt)) {
                    // 记录致命错误并返回
                    res.hasFatalError = true;
                    res.errors.push_back({ 0, String("格式字符串中括号不匹配") });
                    return res;
                }
            }
            catch (const std::exception& ex) {
                res.hasFatalError = true;
                res.errors.push_back({ 0, String(std::string("ValidateBraces threw: ") + ex.what()) });
                return res;
            }
            catch (...) {
                res.hasFatalError = true;
                res.errors.push_back({ 0, String("ValidateBraces threw unknown exception") });
                return res;
            }

            size_t pos = 0;
            String content;
            bool isPh = false;

            std::set<int> usedIndices;      // 已使用索引
            int nextAutoIndex = 0;          // 下一个自动分配索引

            while (true) {
                size_t tokenStart = pos;
                try {
                    bool ok = ExtractNextToken(fmt, pos, content, isPh);
                    if (!ok) break;

                    Token t;
                    t.isPlaceholder = isPh;
                    t.position = tokenStart;

                    if (isPh) {
                        try {
                            // ParsePlaceholder 可能抛出异常 -> 捕获并记录到 res
                            FormatSpec spec = ParsePlaceholder(content);

                            // 智能索引逻辑
                            if (spec.HasExplicitIndex()) {
                                usedIndices.insert(spec.GetIndex());
                            }
                            else {
                                while (usedIndices.count(nextAutoIndex)) ++nextAutoIndex;
                                spec.SetIndex(nextAutoIndex);
                                usedIndices.insert(nextAutoIndex++);
                            }

                            spec.SetRaw(content);
                            t.spec = std::move(spec);
                        }
                        catch (const std::exception& ex) {
                            // 将解析错误作为非致命错误记录，并尝试继续（或根据需要标记致命）
                            res.errors.push_back({ tokenStart, String(std::string("ParsePlaceholder error: ") + ex.what()) });
                            // 将该 token 标记为无效占位符，以便后续 Format 执行阶段输出占位符占位符（例如 "{!}"）
                            t.spec.SetValid(false);
                            t.literal = String("{!}");
                            t.isPlaceholder = false; // treat as literal fallback
                        }
                        catch (...) {
                            res.errors.push_back({ tokenStart, String("ParsePlaceholder threw unknown exception") });
                            t.spec.SetValid(false);
                            t.literal = String("{!}");
                            t.isPlaceholder = false;
                        }
                    }
                    else {
                        t.literal = content;
                    }

                    res.tokens.push_back(std::move(t));
                }
                catch (const std::exception& ex) {
                    // ExtractNextToken 或其他底层函数抛出异常，记录为致命错误并停止解析
                    res.hasFatalError = true;
                    res.errors.push_back({ tokenStart, String(std::string("Format parsing failed: ") + ex.what()) });
                    break;
                }
                catch (...) {
                    res.hasFatalError = true;
                    res.errors.push_back({ tokenStart, String("Format parsing failed: unknown exception") });
                    break;
                }
            }

            return res;
        }

        // 检查大括号匹配与转义合法性
        bool FormatParser::ValidateBraces(const String& fmt) {
            int depth = 0;
            for (size_t i = 0; i < fmt.Size(); ++i) {
                char32_t c = fmt[i];
                if (c == U'{') {
                    if (i + 1 < fmt.Size() && fmt[i + 1] == U'{') { ++i; continue; }
                    ++depth;
                }
                else if (c == U'}') {
                    if (i + 1 < fmt.Size() && fmt[i + 1] == U'}') { ++i; continue; }
                    if (--depth < 0) return false;
                }
            }
            return depth == 0;
        }

        // 提取下一个 token
        bool FormatParser::ExtractNextToken(const String& fmt, size_t& pos, String& outContent, bool& outIsPlaceholder) {
            if (pos >= fmt.Size()) return false;
            outContent.Clear();

            while (pos < fmt.Size()) {
                char32_t c = fmt[pos];

                if (c == U'{') {
                    if (pos + 1 < fmt.Size() && fmt[pos + 1] == U'{') {
                        // 转义 {{
                        outContent.Append(U'{');
                        pos += 2;
                    }
                    else {
                        // 占位符开始
                        if (!outContent.Empty()) {
                            // 先返回前面的文本
                            outIsPlaceholder = false;
                            return true;
                        }
                        ++pos;
                        // 找闭合 }
                        size_t phStart = pos;
                        while (pos < fmt.Size() && fmt[pos] != U'}') ++pos;
                        if (pos >= fmt.Size()) ThrowFormatError(U"缺少 '}' 结束符", fmt, phStart - 1);
                        outIsPlaceholder = true;
                        outContent = fmt.SubString(phStart, pos - phStart);
                        ++pos;
                        return true;
                    }
                }
                else if (c == U'}') {
                    if (pos + 1 < fmt.Size() && fmt[pos + 1] == U'}') {
                        // 转义 }}
                        outContent.Append(U'}');
                        pos += 2;
                    }
                    else {
                        // 单独的闭括号，说明文本结束
                        if (!outContent.Empty()) {
                            outIsPlaceholder = false;
                            return true;
                        }
                        // 不合法的单 } 在 ValidateBraces 会检查
                        ++pos;
                        outContent.Append(U'}');
                        outIsPlaceholder = false;
                        return true;
                    }
                }
                else {
                    outContent.Append(c);
                    ++pos;
                }
            }

            // 文本剩余
            if (!outContent.Empty()) {
                outIsPlaceholder = false;
                return true;
            }
            return false;
        }

        // 解析占位符
        FormatSpec FormatParser::ParsePlaceholder(const String& inside) {
            FormatSpec spec;
            size_t i = 0;

            SkipSpaces(inside, i);
            auto idx = ParseNumber(inside, i);
            if (idx) spec.SetIndex(*idx, true);

            SkipSpaces(inside, i);
            if (i < inside.Size() && inside[i] == U':') {
                ++i;
                ParseFormatOptions(inside, i, spec);
            }
            SkipSpaces(inside, i);
            if (i != inside.Size())
                ThrowFormatError(U"占位符中存在无效字符", inside, i);

            return spec;
        }

        // 跳过空格
        void FormatParser::SkipSpaces(const String& s, size_t& i) {
            while (i < s.Size() && (s[i] == U' ' || s[i] == U'\t')) ++i;
        }

        // 解析整数数字
        std::optional<int> FormatParser::ParseNumber(const String& s, size_t& i) {
            if (i >= s.Size() || !isdigit((int)s[i])) return std::nullopt;
            int val = 0;
            while (i < s.Size() && isdigit((int)s[i])) {
                val = val * 10 + (s[i] - U'0');
                ++i;
            }
            return val;
        }

        // 解析填充字符（例如 "_>" 或 "'X^"）
        String FormatParser::ParseFillChar(const String& s, size_t& i) {
            if (i >= s.Size()) return String();

            char32_t quote = 0;
            if (s[i] == U'\'' || s[i] == U'"') {
                quote = s[i];
                ++i;
            }

            String fill;
            if (quote) {
                // 多字符或转义解析
                while (i < s.Size()) {
                    char32_t c = s[i];
                    if (c == quote) {
                        ++i; // 结束
                        break;
                    }
                    if (c == U'\\' && i + 1 < s.Size()) {
                        ++i;
                        switch (s[i]) {
                        case U'n': fill += U'\n'; break;
                        case U't': fill += U'\t'; break;
                        case U'r': fill += U'\r'; break;
                        case U'\\': fill += U'\\'; break;
                        case U'\'': fill += U'\''; break;
                        case U'"': fill += U'"'; break;
                        default: fill += s[i]; break;
                        }
                    }
                    else {
                        fill += c;
                    }
                    ++i;
                }
            }
            else if (i + 1 < s.Size() && IsAlignChar(s[i + 1])) {
                fill = String(1, s[i]);
                ++i; // 单字符填充兼容旧写法
            }
            return fill;
        }


        // 判断对齐符号
        bool FormatParser::IsAlignChar(char32_t c) {
            return c == U'<' || c == U'>' || c == U'^' || c == U'=';
        }

        // 解析格式选项
        void FormatParser::ParseFormatOptions(const String& s, size_t& i, FormatSpec& spec) {
            // 填充 + 对齐
            String fill = ParseFillChar(s, i);
            // 尝试解析对齐字符
            char32_t align = 0;
            if (i < s.Size() && IsAlignChar(s[i])) {
                align = s[i];
                ++i;
            }

            // 无论 align 是否存在，都先设置 fill
            if (!fill.Empty()) {
                spec.SetFill(fill);
            }

            // 如果存在对齐字符，再设置 align
            if (align) {
                spec.SetAlign(align);
            }

            // 符号
            if (i < s.Size() && (s[i] == U'+' || s[i] == U'-' || s[i] == U' ')) {
                spec.SetSign(s[i]);
                ++i;
            }

            // '#' 标志
            if (i < s.Size() && s[i] == U'#') {
                spec.SetAlternateForm(true);
                ++i;
            }

            // '0' 填充
            if (i < s.Size() && s[i] == U'0') {
                spec.SetZeroPad(true);
                ++i;
            }

            // 宽度
            if (auto w = ParseNumber(s, i)) spec.SetWidth(*w);

            // 精度
            if (i < s.Size() && s[i] == U'.') {
                ++i;
                if (auto p = ParseNumber(s, i)) spec.SetPrecision(*p);
                else ThrowFormatError(U"精度值无效", s, i);
            }

            // 类型
            if (i < s.Size() && IsTypeChar(s[i])) {
                spec.SetType(s[i]);
                ++i;

                // 类型扩展参数
                if (i < s.Size()) {
                    String expand = s.SubString(i, s.Size() - 1);
                    spec.SetTypeExpand(expand);
                    i = s.Size(); // 消耗到末尾
                }
            }
        }

        // 检查类型标识
        bool FormatParser::IsTypeChar(char32_t c) {
            static const char32_t types[] = {
                U's', U'S',     // 字符串
                U'd', U'i',     // 十进制整数
                U'o', U'O',           // 八进制
                U'u',           // 用户自定义类型
                U'x', U'X',     // 十六进制
                U'b', U'B',           // 二进制
                U'f', U'F',     // 浮点数
                U'e', U'E',     // 科学计数法
                U'g', U'G',     // 通用浮点
                U'c',           // 字符
                U'p', U'P',           // 指针
                U't', U'T',     // 时间
                U'%'            // 百分比
            };
            for (auto t : types) if (c == t) return true;
            return false;
        }

        // 抛出格式异常
        [[noreturn]] void FormatParser::ThrowFormatError(const String& msg, const String& context, size_t pos) {
            std::stringstream ss;
            ss << "[FormatParserError] " << msg.ToStdString()
                << " 位置=" << pos
                << " 上下文=\"" << context.ToStdString() << "\"";
            throw std::runtime_error(ss.str());
        }
	}
}
