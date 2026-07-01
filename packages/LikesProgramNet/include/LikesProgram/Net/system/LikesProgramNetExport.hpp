#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_NET_SHARED)
# if defined(LIKESPROGRAM_NET_EXPORTS)
#  define LIKESPROGRAM_NET_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_NET_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_NET_API
#endif
