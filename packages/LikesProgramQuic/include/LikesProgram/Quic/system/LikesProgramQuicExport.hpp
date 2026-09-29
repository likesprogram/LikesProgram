#pragma once

#if defined(_WIN32) && defined(LIKESPROGRAM_QUIC_SHARED)
# if defined(LIKESPROGRAM_QUIC_EXPORTS)
#  define LIKESPROGRAM_QUIC_API __declspec(dllexport)
# else
#  define LIKESPROGRAM_QUIC_API __declspec(dllimport)
# endif
#else
# define LIKESPROGRAM_QUIC_API
#endif
