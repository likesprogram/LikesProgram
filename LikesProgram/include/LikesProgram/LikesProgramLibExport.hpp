#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_EXPORT) // Windows 导入导出
# ifdef LIKESPROGRAM_LIB_EXPORTS
#  define LIKESPROGRAM_API __declspec(dllexport) // 导出
# else
#  define LIKESPROGRAM_API __declspec(dllimport) // 导入
# endif
#else
# define LIKESPROGRAM_API // 其他平台 空宏
#endif