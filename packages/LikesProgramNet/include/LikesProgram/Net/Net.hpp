#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/Client.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/IOEvent.hpp>
#include <LikesProgram/Net/Poller.hpp>
#include <LikesProgram/Net/Server.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <LikesProgram/Net/TcpTransport.hpp>
#include <LikesProgram/Net/Transport.hpp>
#include <LikesProgram/Net/UdpTransport.hpp>

namespace LikesProgram {
    namespace Net {
        // 返回 Net 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_NET_API const char* PackageName() noexcept;
        // 返回 Net 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_NET_API const char* PackageVersion() noexcept;
        // 表示 Net 包目标已被成功链接到当前进程。
        LIKESPROGRAM_NET_API bool PackageAvailable() noexcept;
    }
}
