#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_EXPORT) // Windows ���뵼��
# ifdef LIKESPROGRAM_LIB_EXPORTS
#  define LIKESPROGRAM_API __declspec(dllexport) // ����
# else
#  define LIKESPROGRAM_API __declspec(dllimport) // ����
# endif
#else
# define LIKESPROGRAM_API // ����ƽ̨ �պ�
#endif