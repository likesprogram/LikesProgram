#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>

#include <LikesProgram/Net/SocketType.hpp>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class IocpOperationKind {
                Connect,
                Accept,
                TcpRead,
                TcpWrite,
                UdpRead,
                UdpWrite
            };

            // 内核完成前保持地址稳定，派生类型在 cpp 中持有专属资源。
            struct IocpOperation {
                OVERLAPPED m_overlapped{};
                IocpOperationKind m_kind{};
                std::uint64_t m_id = 0;
                std::uint64_t m_ownerGeneration = 0;
                SocketType m_fd = kInvalidSocket;
                bool m_cancelRequested = false;
                bool m_syntheticCompletion = false;
                int m_syntheticError = 0;

                virtual ~IocpOperation() = default;
            };
        }
    }
}
