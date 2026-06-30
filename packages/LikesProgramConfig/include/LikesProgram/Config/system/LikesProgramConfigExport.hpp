#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_CONFIG_SHARED)
# if defined(LIKESPROGRAM_CONFIG_EXPORTS)
#  define LIKESPROGRAM_CONFIG_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_CONFIG_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_CONFIG_API
#endif
