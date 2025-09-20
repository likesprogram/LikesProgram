#pragma once
#include "LikesProgramLibExport.hpp"
#include "String.hpp"

namespace LikesProgram {
	namespace CoreUtils {
        // ���õ�ǰ�߳���
		LIKESPROGRAM_API void SetCurrentThreadName(const LikesProgram::String& name);

        // ��ȡ��ǰ�߳���
		LIKESPROGRAM_API LikesProgram::String GetCurrentThreadName();

		// ��ȡ���� MAC ��ַ
		LIKESPROGRAM_API LikesProgram::String GetMACAddress();

		// ��ȡ���� IP ��ַ
		LIKESPROGRAM_API LikesProgram::String GetLocalIPAddress();

		// ���� UUID
		LIKESPROGRAM_API LikesProgram::String GenerateUUID(LikesProgram::String prefix = u"");
	}
}