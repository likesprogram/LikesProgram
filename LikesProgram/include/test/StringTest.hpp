#pragma once
#include <iostream>
#include "../LikesProgram/String.hpp"
using namespace LikesProgram;

namespace StringTest {
    // 输入输出测试
    void OutAndIn() {
        std::cout << "===== 输出输出示例 =====" << std::endl;
#ifdef _WIN32
        String str1("", String::Encoding::GBK); // Windows 控制台输入需要先设置编码为 GBK，Linux 控制台输入可使用默认 UTF-8
#else
        String str1; // Linux 控制台输入可使用默认 UTF-8
#endif
        std::cout << "请输入字符串[cin]：";
        std::cin >> str1;
        std::cout << "[cout]：" << str1 << "\n";

        String str2;
        std::cout << "请输入字符串[wcin]：";
        std::wcin >> str2;
        std::wcout << "[wcout]：" << str2 << "\n";
    }

    // 格式化测试
    void Format() {
        std::cout << "===== 格式化示例 =====" << std::endl;
        String s1 = String::Format(u"值: {:010}", 123);
        String s2 = String::Format(u"十六进制: {:#08X}", 255);
        String s3 = String::Format(u"浮点数: {:.3f}", 3.14159);
        String s4 = String::Format(u"左对齐: {:<5}", String(u"测试dfgdfgdfgdfgdfg"));
        String s5 = String::Format(u"参数索引: {1:02} + {0:02} = {2:02}", 2, 3, 5);
//        String s1 = String::Format(u8"utf-8格式化: %03d.%03d", 1, 2);        // const char8_t*
//        String s2 = String::Format(u"utf-16格式化: %03d.%03d", 3, 4);      // const char16_t*
//        String s3 = String::Format(U"utf-32格式化: %03d.%03d", 5, 6);      // const char32_t*
//        String s4 = String::Format(L"宽字符串格式化: %03d.%03d", 7, 8);    // 宽字符串
//        //String s4 = String::Format(u"宽字符串格式化: %03d.%03d", String(u"12"), 8);    // 宽字符串
//
#ifdef _WIN32
        std::cout << s1.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << s2.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << s3.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << s4.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << s5.ToStdString(String::Encoding::GBK) << "\n";
#else
        std::cout << s1 << "\n";
        std::cout << s2 << "\n";
        std::cout << s3 << "\n";
        std::cout << s4 << "\n";
        std::cout << s5 << "\n";
#endif
        std::cout << std::endl;
    }

    void Test() {
        Format();
        OutAndIn();
        std::cout << "===== 其他示例 =====" << std::endl;
        // 构造测试
        String s1(u"Hello 世界");      // UTF-16
        String s2("hello world");      // UTF-8 默认
        String s3(U"🌟星");           // UTF-32 emoji + 中文
        String s4 = s1;                // 拷贝构造
        String s5 = std::move(s2);     // 移动构造

        std::cout << "s1 size: " << s1.Size() << "\n"; // Unicode code points
        std::cout << "s3 size: " << s3.Size() << "\n";

        String sAdd1 = u"LikesProgram 字符串 - " + s1;
        String sAdd2 = s1 + u" - LikesProgram 字符串";
#ifdef _WIN32
        std::cout << "sAdd1: " << sAdd1.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << "sAdd2: " << sAdd2.ToStdString(String::Encoding::GBK) << "\n";
#else
        std::cout << "sAdd1: " << sAdd1 << "\n";
        std::cout << "sAdd2: " << sAdd2 << "\n";
#endif

        // 拼接
        s1.Append(s3);
        std::cout << "After append, s1 size: " << s1.Size() << "\n";

        // 子串
        String sub = s1.SubString(0, 5);
        std::cout << "SubString(0,5) size: " << sub.Size() << "\n";

        // 大小写转换
        String upper = s1.ToUpper();
        String lower = s1.ToLower();
#ifdef _WIN32
        std::cout << "upper: " << upper.ToStdString(String::Encoding::GBK) << "\n";
        std::cout << "lower: " << lower.ToStdString(String::Encoding::GBK) << "\n";
#else
        std::cout << "upper: " << upper.ToStdString() << "\n";
        std::cout << "lower: " << lower.ToStdString() << "\n";
#endif

        // 查找
        size_t idx = s1.Find(String(u"世界"));
        std::cout << "Find '世界': " << idx << "\n";

        size_t last_idx = s1.LastFind(String(u"星"));
        std::cout << "LastFind '星': " << last_idx << "\n";

        // StartsWith / EndsWith
        std::cout << "StartsWith 'Hello': " << s1.StartsWith(String(u"Hello")) << "\n";
        std::cout << "EndsWith '星': " << s1.EndsWith(String(U"星")) << "\n";

        // 忽略大小写比较
        String cmp1("Test");
        String cmp2("tEsT");
        std::cout << "EqualsIgnoreCase: " << cmp1.EqualsIgnoreCase(cmp2) << "\n";

        // 迭代器
        std::cout << "Iterate code points: ";
        for (auto cp : s1) {
            std::cout << std::hex << "U+" << static_cast<uint32_t>(cp) << " ";
        }
        std::cout << "\n";

        // 分割
        String s6("a,b,c,d");
        auto parts = s6.Split(String(u","));
        std::cout << "Split: ";
        for (auto& p : parts) {
            std::cout << p.ToStdString() << " ";
        }
        std::cout << "\n";
    }
}
