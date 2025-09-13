#pragma once
#include "../LikesProgramLibExport.hpp"
#include <cstdint>

namespace LikesProgram {
	namespace Unicode {
		namespace Case {
            // BMP���ַ�ת��Ϊ��д
            LIKESPROGRAM_API uint16_t BMPToUpper(uint16_t c);

            // BMP���ַ�ת��ΪСд
            LIKESPROGRAM_API uint16_t BMPToLower(uint16_t c);

            // SMP���ַ�ת��Ϊ��д
            LIKESPROGRAM_API uint32_t SMPToUpper(uint32_t c);

            // SMP���ַ�ת��ΪСд
            LIKESPROGRAM_API uint32_t SMPToLower(uint32_t c);
		}
	}
}