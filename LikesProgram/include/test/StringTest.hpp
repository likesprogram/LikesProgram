#pragma once
#include <iostream>
#include "../LikesProgram/String.hpp"

namespace StringTest {
    // 输入输出测试
    void OutAndIn() {
        std::cout << "===== 输出输出示例 =====" << std::endl;
#ifdef _WIN32
        LikesProgram::String str1("", LikesProgram::String::Encoding::GBK); // Windows 控制台输入需要先设置编码为 GBK，Linux 控制台输入可使用默认 UTF-8
#else
        LikesProgram::String str1; // Linux 控制台输入可使用默认 UTF-8
#endif
        std::cout << "请输入字符串[cin]：";
        std::cin >> str1;
        std::cout << "[cout]：" << str1 << "\n";

        LikesProgram::String str2;
        std::cout << "请输入字符串[wcin]：";
        std::wcin >> str2;
        std::wcout << "[wcout]：" << str2 << "\n";
    }

    void Test() {
        OutAndIn();
        std::cout << "===== 其他示例 =====" << std::endl;
        // 构造测试
        LikesProgram::String s1(u"Hello 世界");      // UTF-16
        LikesProgram::String s2("hello world");      // UTF-8 默认
        LikesProgram::String s3(U"🌟星");           // UTF-32 emoji + 中文
        LikesProgram::String s4 = s1;                // 拷贝构造
        LikesProgram::String s5 = std::move(s2);     // 移动构造

        std::cout << "s1 size: " << s1.Size() << "\n"; // Unicode code points
        std::cout << "s3 size: " << s3.Size() << "\n";

        // 拼接
        s1.Append(s3);
        std::cout << "After append, s1 size: " << s1.Size() << "\n";

        // 子串
        LikesProgram::String sub = s1.SubString(0, 5);
        std::cout << "SubString(0,5) size: " << sub.Size() << "\n";

        // 大小写转换
        LikesProgram::String upper = s1.ToUpper();
        LikesProgram::String lower = s1.ToLower();
#ifdef _WIN32
        std::cout << "upper: " << upper.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
        std::cout << "lower: " << lower.ToStdString(LikesProgram::String::Encoding::GBK) << "\n";
#else
        std::cout << "upper: " << upper.ToStdString() << "\n";
        std::cout << "lower: " << lower.ToStdString() << "\n";
#endif

        // 查找
        size_t idx = s1.Find(LikesProgram::String(u"世界"));
        std::cout << "Find '世界': " << idx << "\n";

        size_t last_idx = s1.LastFind(LikesProgram::String(u"星"));
        std::cout << "LastFind '星': " << last_idx << "\n";

        // StartsWith / EndsWith
        std::cout << "StartsWith 'Hello': " << s1.StartsWith(LikesProgram::String(u"Hello")) << "\n";
        std::cout << "EndsWith '星': " << s1.EndsWith(LikesProgram::String(U"星")) << "\n";

        // 忽略大小写比较
        LikesProgram::String cmp1("Test");
        LikesProgram::String cmp2("tEsT");
        std::cout << "EqualsIgnoreCase: " << cmp1.EqualsIgnoreCase(cmp2) << "\n";

        // 迭代器
        std::cout << "Iterate code points: ";
        for (auto cp : s1) {
            std::cout << std::hex << "U+" << static_cast<uint32_t>(cp) << " ";
        }
        std::cout << "\n";

        // 分割
        LikesProgram::String s6("a,b,c,d");
        auto parts = s6.Split(LikesProgram::String(u","));
        std::cout << "Split: ";
        for (auto& p : parts) {
            std::cout << p.ToStdString() << " ";
        }
        std::cout << "\n";
    }
}
