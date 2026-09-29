#pragma once
#include <LikesProgram/Http/system/LikesProgramHttpExport.hpp>
#include <LikesProgram/Http/Http1.hpp>
#include <LikesProgram/Http/HttpBodyBackpressure.hpp>
#include <LikesProgram/Http/HttpBodyBudget.hpp>
#include <LikesProgram/Http/HttpBodyCancellation.hpp>
#include <LikesProgram/Http/HttpBodyProducer.hpp>
#include <LikesProgram/Http/HttpBodySink.hpp>
#include <LikesProgram/Http/HttpConnectionPool.hpp>
#include <LikesProgram/Http/HttpHeaderBlock.hpp>
#include <LikesProgram/Http/HttpObservability.hpp>
#include <LikesProgram/Http/Http2.hpp>
#include <LikesProgram/Http/Http2Hpack.hpp>
#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/Http3Control.hpp>
#include <LikesProgram/Http/Http3Qpack.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>
#include <LikesProgram/Http/Http3QuicRequestStreamBridge.hpp>
#include <LikesProgram/Http/Http3QuicControlStreamBridge.hpp>
#include <LikesProgram/Http/HttpAltSvc.hpp>
#include <LikesProgram/Http/HttpSession.hpp>
#include <LikesProgram/Http/HttpWebsite.hpp>

namespace LikesProgram {
    namespace Http {
        // 返回 Http 包名，用于测试、示例和诊断输出。
        LIKESPROGRAM_HTTP_API const char* PackageName() noexcept;

        // 返回 Http 包当前跟随的 LikesProgram 统一版本号。
        LIKESPROGRAM_HTTP_API const char* PackageVersion() noexcept;

        // 表示 Http 包目标已被成功链接到当前进程。
        LIKESPROGRAM_HTTP_API bool PackageAvailable() noexcept;
    }
}
