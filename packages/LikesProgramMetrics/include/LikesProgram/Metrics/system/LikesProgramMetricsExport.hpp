#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_METRICS_SHARED)
# if defined(LIKESPROGRAM_METRICS_EXPORTS)
#  define LIKESPROGRAM_METRICS_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_METRICS_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_METRICS_API
#endif
