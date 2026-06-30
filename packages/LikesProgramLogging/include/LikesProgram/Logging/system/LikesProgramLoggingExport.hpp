#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_LOGGING_SHARED)
# if defined(LIKESPROGRAM_LOGGING_EXPORTS)
#  define LIKESPROGRAM_LOGGING_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_LOGGING_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_LOGGING_API
#endif
