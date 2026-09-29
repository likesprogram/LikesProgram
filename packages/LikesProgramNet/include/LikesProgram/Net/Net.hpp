#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/BufferChain.hpp>
#include <LikesProgram/Net/BufferLease.hpp>
#include <LikesProgram/Net/BufferSlice.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/Client.hpp>
#include <LikesProgram/Net/Connection.hpp>
#include <LikesProgram/Net/ConnectionFactory.hpp>
#include <LikesProgram/Net/ConnectionPool.hpp>
#include <LikesProgram/Net/DtlsDatagramBatch.hpp>
#include <LikesProgram/Net/DtlsEngine.hpp>
#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include <LikesProgram/Net/EventLoop.hpp>
#include <LikesProgram/Net/EventLoopGroup.hpp>
#include <LikesProgram/Net/IOEvent.hpp>
#include <LikesProgram/Net/Poller.hpp>
#include <LikesProgram/Net/Protocol.hpp>
#include <LikesProgram/Net/Server.hpp>
#include <LikesProgram/Net/SocketType.hpp>
#include <LikesProgram/Net/TlsEngine.hpp>
#include <LikesProgram/Net/TlsEngineFactory.hpp>

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
