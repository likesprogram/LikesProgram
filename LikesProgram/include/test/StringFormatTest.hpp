#pragma once
#include <iostream>
#include "../LikesProgram/String.hpp"
#include "../LikesProgram/stringFormat/FormatParser.hpp"
#include "../LikesProgram/Logger.hpp"
#include "../LikesProgram/stringFormat/FormatInternal.hpp"
#include "../LikesProgram/time/Timer.hpp"
using namespace LikesProgram;
using namespace LikesProgram::Time;
using namespace LikesProgram::StringFormat;
namespace StringFormatTest {
	void FormatParserTest(const String& fmt) {
        try {
            FormatParser parser;
            auto result = parser.Parse(fmt);
            String log;

            if (result.hasFatalError) {
                log += u"致命错误:\n";
                for (auto& e : result.errors) {
                    log += u"  位置=";
                    log += String::Format(u"{}", e.position);
                    log += u"  ";
                    log += e.message;
                    log += u"\n";
                }
                LOG_DEBUG(log);
                return;
            }

            for (auto& tok : result.tokens) {
                if (tok.isPlaceholder) {
                    auto& s = tok.spec;
                    log += u"\r\n[占位符] idx=";
                    log += String::Format(u"{}", (s.GetIndex()));
                    log += u" explicit=";
                    log += s.HasExplicitIndex() ? u"true" : u"false";
                    log += u" fill=";
                    log += s.GetFill();
                    log += u" align=";
                    log += String(1, static_cast<char16_t>(s.GetAlign()));
                    log += u" sign=";
                    log += String(1, static_cast<char16_t>(s.GetSign()));
                    log += u" #=";
                    log += s.GetAlternateForm() ? u"true" : u"false";
                    log += u" 0pad=";
                    log += s.GetZeroPad() ? u"true" : u"false";

                    log += u" width=";
                    if (s.GetWidth()) log += String::Format(u"{}", (*s.GetWidth()));
                    else log += u"(none)";

                    log += u" precision=";
                    if (s.GetPrecision()) log += String::Format(u"{}", (*s.GetPrecision()));
                    else log += u"(none)";

                    log += u" type=";
                    log += String(1, static_cast<char16_t>(s.GetType()));
                    log += u" typeExpand=";
                    log += s.GetTypeExpand();
                    log += u" valid=";
                    log += s.IsValid() ? u"true" : u"false";
                    log += u" raw=";
                    log += s.GetRaw();
                    log += u"\r\n";
                }
                else {
                    log += u"\r\n[文本]";
                    log += tok.literal;
                    log += u"\r\n";
                }
            }

            if (!result.errors.empty()) {
                log += u"警告/错误信息:\n";
                for (auto& e : result.errors) {
                    log += u"  ";
                    log += e.message;
                    log += u"\n";
                }
            }

            LOG_DEBUG(log);
        }
        catch (const std::exception& ex) {
            LOG_DEBUG(String(u"异常: ") + String(ex.what()));
        }
	}

    void Test() {
        // 初始化日志
#ifdef _DEBUG
        auto& logger = LikesProgram::Logger::Instance(true, true);
#else
        auto& logger = LikesProgram::Logger::Instance(true);
#endif

#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
#ifdef _DEBUG
        logger.SetLevel(LikesProgram::Logger::LogLevel::Debug);
#else
        logger.SetLevel(LikesProgram::Logger::LogLevel::Info);
#endif
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台
        
        // 基本索引和格式化
        FormatParserTest(u"Hello {0}, number={1:d05}");          // 占位符索引 + 数字格式化（宽度5，补0）

        FormatParserTest(U"Hello {0}, number={1:'**'5d}");

        // 时间格式化
        FormatParserTest(u"Time={0:tYYYY-MM-DD hh:mm:ss}");     // 时间类型占位符 + 自定义时间格式

        // 自定义类型
        FormatParserTest(u"Custom={0:uMysd}");                 // 自定义类型占位符

        // 花括号转义
        FormatParserTest(u"Escape {{}} test");                 // 文本中转义大括号，期望输出 {}

        // 多个连续占位符
        FormatParserTest(u"{0} {1} {2}");                       // 简单多个占位符
        FormatParserTest(u"{2}{}{:02d}");      // -> {2}{0}{1}，显式锚点2，其余按序补齐
        FormatParserTest(u"{2}{}{}{ }{}"); // -> {2}{0}{1}{3}，自动分配未使用索引
        FormatParserTest(u"{2}{}{}{}{ }{}"); // -> {2}{0}{1}{3}{4}，完全补齐
        FormatParserTest(u"{} {2} {}");             // -> {0}{2}{1}，智能跳过已显式索引
        FormatParserTest(u"{1}{}{1}{}{ }{}"); // -> {1}{0}{1}{2}{3}，显式索引可重复使用


        // 占位符带填充和对齐
        FormatParserTest(u"Aligned: '{:*>10}' '{1:<10}' '{:^10}'");

        // 占位符带符号和精度
        FormatParserTest(u"Number: {0:+08.2f} {1:-8.3f}");

        // 占位符带类型扩展
        FormatParserTest(u"Hex: {0:#x} {1:#X}");              // 数字类型转换

        // 错误情况测试
        FormatParserTest(u"Broken {0:05");                     // 缺少闭括号，应该报错
        FormatParserTest(u"Bad {0:abc!}");                     // 无效格式字符
        FormatParserTest(u"Unmatched { text");                // 单左大括号
        FormatParserTest(u"Unmatched } text");                // 单右大括号
        FormatParserTest(u"Empty {}");                         // 空占位符，视实现是否允许

        // ===========================
        // 1. 基本索引
        // ===========================
        LOG_DEBUG(String::Format(U"Hello {}, {}!", U"World", 123));                   // 自动索引
        LOG_DEBUG(String::Format(U"{1} + {0} = {2}", 2, 3, 5));                        // 显式索引
        LOG_DEBUG(String::Format(U"{2}{}{1}{}", U"X", 7, U"Y"));                       // 混合索引
        LOG_DEBUG(String::Format(U"{2}{}{}{}{}{}", U"X", U"A", U"B", U"C", U"D"));   // 智能分配未使用索引
        LOG_DEBUG(String::Format(U"{} {2} {}", U"first", U"second", U"third"));       // 自动补齐

        // ===========================
        // 2. 数值类型
        // ===========================
        LOG_DEBUG(String::Format(U"Decimal: {:d} {:i}", 123, -456));
        LOG_DEBUG(String::Format(U"Hex: {:x} {:X} {:#x} {:#X}", 255, 255, 255, 255));
        LOG_DEBUG(String::Format(U"Binary: {:b} {:#b}", 10, 10));
        LOG_DEBUG(String::Format(U"Octal: {:o} {:#o}", 63, 63));
        LOG_DEBUG(String::Format(U"Float: {:.2f} {:.3f}", 3.14159, 2.71828));
        LOG_DEBUG(String::Format(U"Scientific: {:.2e} {:.3E}", 1234.5678, 1234.5678));
        LOG_DEBUG(String::Format(U"Percent: {:.2%}", 0.1234));
        LOG_DEBUG(String::Format(U"Sign: {:+d} {:-d} {: d} {:+.2f}", 42, -42, 7, 3.14));

        // ===========================
        // 3. 填充与对齐
        // ===========================
        LOG_DEBUG(String::Format(U"Right: '{:*>10}'", U"R"));
        LOG_DEBUG(String::Format(U"Left: '{:<10}'", U"L"));
        LOG_DEBUG(String::Format(U"Center: '{:^10}'", U"C"));
        LOG_DEBUG(String::Format(U"Multi-fill: '{:'**'^8}' '{:'--'<10}'", U"A", U"B"));
        LOG_DEBUG(String::Format(U"Complex: '{:'░'^9}'", U"X"));

        // ===========================
        // 4. 时间类型 (type=t/T)
        // ===========================
        auto now = std::chrono::system_clock::now();
        LOG_DEBUG(String::Format(U"Time: {:t%Y-%m-%d %H:%M:%S}", now));
        LOG_DEBUG(String::Format(U"Time2: {:t%Y/%m/%d %H:%M:%S %6f}", now));
        LOG_DEBUG(String::Format(U"Time3: {:t%Y/%m/%d %H:%M:%S %f AP}", now));
        LOG_DEBUG(String::Format(U"Time4: {:tYYYY-MM-DD hh:mm:ss SSS}", now));
        LOG_DEBUG(String::Format(U"Time4: {:t%Y-M-D %H:%M:%S SSS AP W WW TZ}", now));
        LOG_DEBUG(String::Format(U"Time5: {:tW WW TZ}", now)); // 星期简写/全称 + 时区

        // ===========================
        // 5. 自定义类型
        // ===========================
        struct Vec2 { double x, y; };

        FormatInternal& fmt = FormatInternal::Instance();

        // 注册自定义格式化器
        fmt.RegisterFormatter("Vec2", [](const Any& val, const FormatSpec& spec) -> String {
            const Vec2& v = std::any_cast<const Vec2&>(val);
            
            return String::Format(u"({:.2f}, {:.2f})", v.x, v.y);
        });

        // 检查是否注册成功
        if (fmt.HasFormatter("Vec2")) {
            LOG_DEBUG(u"Formatter Vec2 registered.");
        }

        Vec2 v{ 1.23, 4.56 };
        LOG_DEBUG(String::Format(U"Custom Vec2: {:uVec2}", v));

        // ===========================
        // 6. 花括号转义
        // ===========================
        LOG_DEBUG(String::Format(U"Escape {{}} test"));

        // ===========================
        // 7. 多字符填充与组合
        // ===========================
        LOG_DEBUG(String::Format(U"Multi-char fill: '{:'**'<6}' '{:'--'^8}' '{:'##'>10}'", U"A", U"B", U"C"));

        // ===========================
        // 8. 空字符串
        // ===========================
        LOG_DEBUG(String::Format(U"Empty string: '{}'", U""));

        // ===========================
        // 9. 指针类型
        // ===========================
        int val = 42;
        int* ptr = &val;
        LOG_DEBUG(String::Format(U"Pointer: {:p}", ptr));

        // ===========================
        // 10. 错误回退
        // ===========================
        LOG_DEBUG(String::Format(U"Missing brace {0:05", 10));   // 缺少闭括号
        LOG_DEBUG(String::Format(U"Invalid type {0:abc!}", 10)); // 无效格式字符
        LOG_DEBUG(String::Format(U"Unmatched { text"));
        LOG_DEBUG(String::Format(U"Unmatched } text"));
        LOG_DEBUG(String::Format(U"Empty {}", U""));

        // ===========================
        // 11. 智能索引重复测试
        // ===========================
        LOG_DEBUG(String::Format(U"{1}{}{1}{}{}{}", U"A", U"B", U"C", U"D", U"E"));

        logger.Shutdown();
    }
}