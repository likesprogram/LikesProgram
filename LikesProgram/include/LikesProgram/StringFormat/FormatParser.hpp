#pragma once
#include "../system/LikesProgramLibExport.hpp"
#include <optional>
#include "../String.hpp"
#include "FormatSpec.hpp"

namespace LikesProgram {
	namespace StringFormat {
        class LIKESPROGRAM_API FormatParser {
        public:
            // 解析单元
            struct Token {
                bool isPlaceholder = false; // true 表示 {} 占位符
                String literal;             // 普通文本或占位符原始内容
                FormatSpec spec;            // 占位符解析后的规格信息
                size_t position = 0;        // 格式串中起始位置，用于错误定位
            };

            // 错误信息
            struct Error {
                size_t position = 0;    // 错误位置
                String message;         // 错误描述
            };

            // 解析结果
            struct Result {
                std::vector<Token> tokens;  // 成功解析的结果
                std::vector<Error> errors;  // 非致命错误集合
                bool hasFatalError = false; // 若存在致命错误（无法继续解析）
            };

        public:
            FormatParser() = default;
            ~FormatParser() = default;

            // 主解析入口
            // 解析整个格式字符串，返回解析结果（包含错误信息）
            Result Parse(const String& fmt);

        private: // 内部工具函数

            // 检查大括号是否匹配
            static bool ValidateBraces(const String& fmt);

            // 提取下一个 {} 或文本片段
            static bool ExtractNextToken(const String& fmt, size_t& pos, String& outContent, bool& outIsPlaceholder);

            // 从占位符内容中解析 FormatSpec
            static FormatSpec ParsePlaceholder(const String& inside);

            // 跳过空白
            static void SkipSpaces(const String& s, size_t& i);

            // 提取数字（用于 index / width / precision）
            static std::optional<int> ParseNumber(const String& s, size_t& i);

            // 提取填充字符（可能为 ' 或 " 包裹）
            static String ParseFillChar(const String& s, size_t& i);

            // 检查是否为合法对齐符号
            static bool IsAlignChar(char32_t c);

            // 从 spec 内提取 [align][sign][#][0][width][.precision][type]
            static void ParseFormatOptions(const String& s, size_t& i, FormatSpec& spec);

            // 检查字符是否属于类型标识 (f, d, s, x 等)
            static bool IsTypeChar(char32_t c);

            // 安全取字符（避免越界）
            // static char32_t SafeChar(const String& s, size_t i);

            // 抛出带上下文信息的异常
            [[noreturn]] static void ThrowFormatError(const String& msg, const String& context, size_t pos);
        };
	}
}
