#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_CONFIG_SHARED)
# if defined(LIKESPROGRAM_CONFIG_EXPORTS)
#  define LIKESPROGRAM_CONFIG_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_CONFIG_API __declspec(dllimport)
# endif
#elif defined(LIKESPROGRAM_CONFIG_SHARED) && (defined(__GNUC__) || defined(__clang__))
# define LIKESPROGRAM_CONFIG_API __attribute__((visibility("default")))
#else
# define LIKESPROGRAM_CONFIG_API
#endif
