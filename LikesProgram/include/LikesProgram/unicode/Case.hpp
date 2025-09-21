#pragma once
#include "../LikesProgramLibExport.hpp"
#include <cstdint>

namespace LikesProgram {
	namespace Unicode {
		namespace Case {
            // BMP中字符转换为大写
            LIKESPROGRAM_API uint16_t BMPToUpper(uint16_t c);

            // BMP中字符转换为小写
            LIKESPROGRAM_API uint16_t BMPToLower(uint16_t c);

            // SMP中字符转换为大写
            LIKESPROGRAM_API uint32_t SMPToUpper(uint32_t c);

            // SMP中字符转换为小写
            LIKESPROGRAM_API uint32_t SMPToLower(uint32_t c);
		}
	}
}