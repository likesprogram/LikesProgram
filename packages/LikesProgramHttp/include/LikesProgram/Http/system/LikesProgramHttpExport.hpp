#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_HTTP_SHARED)
# if defined(LIKESPROGRAM_HTTP_EXPORTS)
#  define LIKESPROGRAM_HTTP_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_HTTP_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_HTTP_API
#endif
