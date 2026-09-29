#pragma once

#include <LikesProgram/Net/BufferLease.hpp>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            struct BufferLeaseAccess {
                using ReleaseFunction = void(*)(void*, std::uint32_t) noexcept;

                // 把平台池中的一段 buffer 包装为 move-only lease。
                static BufferLease Adopt(
                    std::uint8_t* data,
                    std::size_t len,
                    void* owner,
                    std::uint32_t token,
                    ReleaseFunction releaseFunction) noexcept {
                    return BufferLease(data, len, owner, token, releaseFunction);
                }
            };
        }
    }
}
