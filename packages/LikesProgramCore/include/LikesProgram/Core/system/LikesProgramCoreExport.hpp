#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_CORE_SHARED)
# if defined(LIKESPROGRAM_CORE_EXPORTS)
#  define LIKESPROGRAM_CORE_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_CORE_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_CORE_API
#endif
