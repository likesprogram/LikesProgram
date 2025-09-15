#pragma once
#include "LikesProgramLibExport.hpp"
#include "String.hpp"

namespace LikesProgram {
	namespace CoreUtils {
        // 设置当前线程名
		LIKESPROGRAM_API void SetCurrentThreadName(const String& name);

        // 获取当前线程名
		LIKESPROGRAM_API String GetCurrentThreadName();

		// 获取本机 MAC 地址
		LIKESPROGRAM_API String GetMACAddress();

		// 获取本机 IP 地址
		LIKESPROGRAM_API String GetLocalIPAddress();

		// 生成 UUID
		LIKESPROGRAM_API String GenerateUUID(String prefix);
	}
}