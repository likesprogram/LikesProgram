#pragma once
#include "../LikesProgramLibExport.hpp"
#include <string>

namespace LikesProgram {
    namespace Unicode {
        namespace Convert {
            // �� UTF-8 ת��Ϊ UTF-16
            LIKESPROGRAM_API std::u16string Utf8ToUtf16(const std::u8string& utf8);

            // �� UTF-32 ת��Ϊ UTF-16
            LIKESPROGRAM_API std::u16string Utf32ToUtf16(const std::u32string& utf32);

            // �� GBK ת��Ϊ UTF-16
            LIKESPROGRAM_API std::u16string GbkToUtf16(const std::string& gbk);

            // �� UTF-16 ת��Ϊ UTF-8
            LIKESPROGRAM_API std::u8string Utf16ToUtf8(const std::u16string& utf16);

            // �� UTF-16 ת��Ϊ UTF-32
            LIKESPROGRAM_API std::u32string Utf16ToUtf32(const std::u16string& utf16);

            // �� UTF-16 ת��Ϊ GBK
            LIKESPROGRAM_API std::string Utf16ToGbk(const std::u16string& utf16);
        }
    }
}