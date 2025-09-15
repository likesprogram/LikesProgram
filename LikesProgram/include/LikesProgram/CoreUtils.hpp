#pragma once
#include "LikesProgramLibExport.hpp"
#include "String.hpp"

namespace LikesProgram {
	namespace CoreUtils {
        // ���õ�ǰ�߳���
		LIKESPROGRAM_API void SetCurrentThreadName(const String& name);

        // ��ȡ��ǰ�߳���
		LIKESPROGRAM_API String GetCurrentThreadName();

		// ��ȡ���� MAC ��ַ
		LIKESPROGRAM_API String GetMACAddress();

		// ��ȡ���� IP ��ַ
		LIKESPROGRAM_API String GetLocalIPAddress();

		// ���� UUID
		LIKESPROGRAM_API String GenerateUUID(String prefix);
	}
}