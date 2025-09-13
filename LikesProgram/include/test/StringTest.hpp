//#pragma once
//#include <iostream>
//#include "../LikesProgram/String.hpp"
//
//namespace StringTest {
//
//    // 基础构造与访问
//    void BasicOps() {
//        std::cout << "===== 基本接口测试 =====" << std::endl;
//
//        LikesProgram::String s1(u8"Hello 世界");
//        std::cout << "UTF-8: " << s1.ToUTF8() << std::endl;
//        std::cout << "Length (code points): " << s1.Length() << std::endl;
//        std::wcout << L"ToWString(): " << s1.ToWString() << std::endl;
//
//        std::wstring ws = L"宽字符";
//        LikesProgram::String s2(ws);
//        std::cout << "From wstring -> UTF8: " << s2.ToUTF8() << std::endl;
//
//        std::u32string u32 = U"ABC😊";
//        LikesProgram::String s3(u32);
//        std::cout << "From UTF32 -> UTF8: " << s3.ToUTF8() << std::endl;
//    }
//
//    // 拼接 & 比较
//    void AppendAndCompare() {
//        std::cout << "\n===== 拼接与比较 =====" << std::endl;
//
//        LikesProgram::String a("Hello");
//        LikesProgram::String b(" World");
//
//        auto c = a + b;
//        std::cout << "a + b = " << c.ToUTF8() << std::endl;
//
//        c += LikesProgram::String("😊");
//        std::cout << "After += : " << c.ToUTF8() << std::endl;
//
//        std::cout << "a == b ? " << (a == b) << std::endl;
//        std::cout << "a != b ? " << (a != b) << std::endl;
//    }
//
//    // Substr & 迭代
//    void SubstrAndIter() {
//        std::cout << "\n===== Substr 与 迭代器 =====" << std::endl;
//        LikesProgram::String text("你好世界🌍");
//
//        auto sub = text.Substr(1, 3);
//        std::cout << "Substr(1,3): " << sub.ToUTF8() << std::endl;
//
//        std::cout << "迭代每个码点: ";
//        for (auto cp : text) {
//            std::cout << std::hex << "U+" << static_cast<uint32_t>(cp) << " ";
//        }
//        std::cout << std::dec << std::endl;
//    }
//
//    // 静态转换函数
//    void StaticFuncs() {
//        std::cout << "\n===== 静态转换函数 =====" << std::endl;
//
//        std::string u8 = "测试";
//        std::wstring w = LikesProgram::String::Utf8ToWString(u8);
//        std::cout << "Utf8ToWString -> size: " << w.size() << std::endl;
//
//        std::string back = LikesProgram::String::WStringToUtf8(w);
//        std::cout << "WStringToUtf8 -> " << back << std::endl;
//
//        std::string u8_from32 = LikesProgram::String::UTF32ToString(U"🐱");
//        std::u32string u32 = LikesProgram::String::StringToUTF32("🐱");
//        std::cout << "UTF32ToString: " << u8_from32 << " | size: " << u32.size() << std::endl;
//
//        std::wstring w2 = LikesProgram::String::UTF32ToWString(U"🍀");
//        std::u32string u32_from_w = LikesProgram::String::WStringToUTF32(w2);
//        std::cout << "UTF32 <-> WString OK, codepoints: " << u32_from_w.size() << std::endl;
//    }
//
//    void Test() {
//        BasicOps();
//        AppendAndCompare();
//        SubstrAndIter();
//        StaticFuncs();
//    }
//}
