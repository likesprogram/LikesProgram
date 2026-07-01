#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_THREADING_SHARED)
# if defined(LIKESPROGRAM_THREADING_EXPORTS)
#  define LIKESPROGRAM_THREADING_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_THREADING_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_THREADING_API
#endif
