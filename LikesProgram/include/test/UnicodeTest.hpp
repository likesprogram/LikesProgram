#pragma once
#include <iostream>
#include <iomanip>
#include "../LikesProgram/unicode/Unicode.hpp"

namespace UnicodeTest {
    void TestBMP_AllLatin() {
        std::cout << "\n=== BMP Latin Full ===\n";

        for (char16_t c = u'a'; c <= u'z'; ++c) {
            uint32_t upper = LikesProgram::Unicode::Case::BMPToUpper(c);

            std::cout << (char)c << " -> " << (char)LikesProgram::Unicode::Case::BMPToUpper(c)
            << " : U+" << std::hex << std::uppercase << static_cast<uint32_t>(c)
            << " -> U+" << static_cast<uint32_t>(upper) << "\n";
        }
        for (char16_t c = u'A'; c <= u'Z'; ++c) {
            uint32_t lower = LikesProgram::Unicode::Case::BMPToLower(c);

            std::cout << (char)c << " -> " << (char)LikesProgram::Unicode::Case::BMPToLower(c)
            << " : U+" << std::hex << std::uppercase << static_cast<uint32_t>(c)
            << " -> U+" << static_cast<uint32_t>(lower) << "\n";
        }
    }

    void TestBMP_Extended() {
        std::cout << "\n=== BMP Extended ===\n";
        char16_t samples[] = {
            0x00E1, 0x00C0, 0x00FC, 0x00DF, // á À ü ß
            0x0100, 0x0101, 0x0130, 0x0131, // Ā ā İ ı
            0x03B1, 0x03C9, 0x0391, 0x03A9, // α ω Α Ω
            0x0410, 0x0430                // А а
        };

        for (auto c : samples) {
            uint32_t u = LikesProgram::Unicode::Case::BMPToUpper(c);
            uint32_t l = LikesProgram::Unicode::Case::BMPToLower(c);
            std::cout << "U+"
                << std::hex << std::uppercase << static_cast<uint32_t>(c)
                << " upper->U+" << u
                << " lower->U+" << l
                << "\n";
        }
    }

    void TestBMP() {
        TestBMP_AllLatin();
        TestBMP_Extended();
    }

    void TestSMP() {
        std::cout << "\n=== SMP Tests ===\n";

        uint32_t samples[] = {
            0x10400, 0x10428, 0x104B0, 0x104D8, // Deseret / Osage
            0x118A0, 0x118C0, 0x118BA, 0x118DA, // Warang Citi
            0x16E40, 0x16E60                  // Medefin / Supplementary examples
        };

        for (auto c : samples) {
            uint32_t u = LikesProgram::Unicode::Case::SMPToUpper(c);
            uint32_t l = LikesProgram::Unicode::Case::SMPToLower(c);
            std::cout << "U+"
                << std::hex << std::uppercase << c
                << " -> upper: U+" << u
                << " lower: U+" << l
                << "\n";
        }
    }

    void ValidateSMP() {
        struct TestCase {
            uint32_t input;
            uint32_t expected_upper;
            uint32_t expected_lower;
        };

        TestCase test_cases[] = {
            {0x10400, 0x10400, 0x10428},
            {0x10428, 0x10400, 0x10428},
            {0x104B0, 0x104B0, 0x104D8},
            {0x104D8, 0x104B0, 0x104D8},
            {0x118A0, 0x118A0, 0x118C0},
            {0x118C0, 0x118A0, 0x118C0},
            {0x118BA, 0x118BA, 0x118DA},
            {0x118DA, 0x118BA, 0x118DA},
            {0x16E40, 0x16E40, 0x16E60},
            {0x16E60, 0x16E40, 0x16E60}
        };

        std::cout << "\n=== SMP Validation ===\n";

        for (auto& tc : test_cases) {
            uint32_t u = LikesProgram::Unicode::Case::SMPToUpper(tc.input);
            uint32_t l = LikesProgram::Unicode::Case::SMPToLower(tc.input);

            std::cout << "0x"
                << std::hex << std::uppercase << tc.input
                << " -> upper: 0x" << u
                << " (expected 0x" << tc.expected_upper << ")"
                << " -> lower: 0x" << l
                << " (expected 0x" << tc.expected_lower << ")";

            if (u != tc.expected_upper || l != tc.expected_lower) {
                std::cout << "  [错]";
            }
            else {
                std::cout << "  [对]";
            }
            std::cout << "\n";
        }
    }
    void TestConvert() {
        std::cout << "\n=== Convert ===\n";
        // UTF-8 → UTF-16
        std::u8string utf8 = u8"你好，Unicode！";
        std::u16string u16 = LikesProgram::Unicode::Convert::Utf8ToUtf16(utf8);
        std::cout << "UTF-8 转 UTF-16 长度: " << std::dec << u16.size() << "\n";

        // UTF-32 → UTF-16
        std::u32string u32 = U"𐐷𐑊"; // Deseret letters
        std::u16string u16_from32 = LikesProgram::Unicode::Convert::Utf32ToUtf16(u32);
        std::cout << "UTF-32 转 UTF-16 长度: " << u16_from32.size() << "\n";

        // UTF-16 → UTF-8
        std::u8string backToUtf8 = LikesProgram::Unicode::Convert::Utf16ToUtf8(u16);
        std::cout << "UTF-16 转 UTF-8: " << backToUtf8.size() << "\n";

        // UTF-16 → UTF-32
        std::u32string backToUtf32 = LikesProgram::Unicode::Convert::Utf16ToUtf32(u16_from32);
        std::cout << "UTF-16 转 UTF-32 长度: " << backToUtf32.size() << "\n";

        // UTF-16 ↔ GBK （仅在 Windows 或启用 iconv 的平台可用）
        std::u16string chinese16 = u"汉字";
        std::string gbk = LikesProgram::Unicode::Convert::Utf16ToGbk(chinese16);
        std::u16string backToUtf16 = LikesProgram::Unicode::Convert::GbkToUtf16(gbk);
        std::cout << "GBK 往返长度: " << backToUtf16.size() << "\n";
        std::cout << "Utf16: ";
        for (char16_t c : backToUtf16) {
            std::cout << std::hex << std::showbase << (uint16_t)c << ' ';
        }
        std::cout << '\n';
        std::cout << "GBK: ";
        for (unsigned char c : gbk) {
            std::cout << std::hex << std::showbase << (int)c << ' ';
        }
        std::cout << std::dec << std::noshowbase << '\n';
    }

    void Test() {
        TestBMP();
        TestSMP();
        ValidateSMP();
        TestConvert();
    }
}
