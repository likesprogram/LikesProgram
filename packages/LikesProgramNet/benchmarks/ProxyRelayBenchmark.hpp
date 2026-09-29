#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace LikesProgram {
    namespace Net {
        namespace Benchmarks {
            enum class ProxyRelayScenario {
                Normal,
                UpstreamEof,
                EarlyQueueOverflow,
                UpstreamWriteOverflow,
                RelayShutdown,
                ConnectFailure,
                ConcurrentClose,
                HandoffClose
            };

            struct ProxyRelayOptions {
                std::size_t payloadBytes = 1024 * 1024; // 单次回环 payload 字节数
                std::size_t earlyQueueLimitBytes = 256 * 1024; // upstream 建连前的硬上限
                std::size_t ingressWriteHighWatermarkBytes = 16 * 1024; // ingress 写高水位
                std::size_t ingressWriteLowWatermarkBytes = 4 * 1024; // ingress 写低水位
                std::size_t ingressMaxPendingWriteBytes = 512 * 1024; // ingress 写硬上限
                std::size_t upstreamWriteHighWatermarkBytes = 16 * 1024; // upstream 写高水位
                std::size_t upstreamWriteLowWatermarkBytes = 4 * 1024; // upstream 写低水位
                std::size_t upstreamMaxPendingWriteBytes = 512 * 1024; // upstream 写硬上限
                std::chrono::milliseconds upstreamStartDelay{ 0 }; // 模拟 upstream 建连延迟
                std::chrono::milliseconds timeout{ 5'000 }; // 单轮 benchmark 超时
                bool forceCloseBeforeUpstream = false; // 测试延迟建连期间的强制收敛
                std::chrono::milliseconds delayWorkerHoldAfterLock{ 0 }; // 锁住 session 后保留窗口
                bool forceCloseAfterDelayWorkerLock = false; // 确定性覆盖 worker 最后引用
                bool holdRemoteStopBeforePost = false; // 保留非 owner 已抢 stop 的竞态窗口
                bool holdUpstreamBeforeOwnerQueue = false; // 保留 upstream handoff 尚未入 owner 队列的窗口
                ProxyRelayScenario scenario = ProxyRelayScenario::Normal; // 确定性关闭场景
            };

            struct ProxyRelayStats {
                double seconds = 0.0; // 本轮完成耗时
                std::uint64_t ingressToUpstreamBytes = 0; // ingress 转发字节
                std::uint64_t upstreamToIngressBytes = 0; // upstream 转发字节
                std::uint64_t backendReceivedBytes = 0; // backend 收到字节
                std::uint64_t clientReceivedBytes = 0; // load client 收到字节
                std::size_t earlyQueuePeakBytes = 0; // 早到队列峰值
                std::size_t earlyQueueLimitBytes = 0; // 早到队列硬上限
                std::uint64_t ingressHighWatermarkEvents = 0; // ingress 写高水位次数
                std::uint64_t ingressLowWatermarkEvents = 0; // ingress 写低水位次数
                std::uint64_t upstreamHighWatermarkEvents = 0; // upstream 写高水位次数
                std::uint64_t upstreamLowWatermarkEvents = 0; // upstream 写低水位次数
                std::uint64_t overflowEvents = 0; // 所有硬上限溢出次数
                std::uint64_t errors = 0; // I/O 或连接错误次数
                bool fullDuplexOverlapObserved = false; // 两方向独立发送同时在途
                bool delayWorkerLocked = false; // 延迟 worker 曾提升 weak session
                bool delayWorkerExited = false; // 延迟 worker 已实际退出
                std::uint64_t delayWorkerJoinEvents = 0; // 外层真实 join 次数
                std::uint64_t ownerStopAttempts = 0; // ingress owner 进入 stop 闸门次数
                std::uint64_t expiredHandoffDrops = 0; // Client 销毁后丢弃的弱 handoff 次数
                std::uint64_t upstreamClientOwnerStopEvents = 0; // ingress owner 停止 upstream 次数
                std::uint64_t upstreamClientWrongThreadStops = 0; // 非 owner 停止次数
                bool resourcesConverged = false; // 所有 owned loop/thread 已停止
                bool cleanShutdown = false; // 是否走稳定收敛路径
                std::string closeReason; // 稳定关闭原因
            };

            // 运行真实 load -> relay -> backend 三段 TCP 回环 benchmark。
            ProxyRelayStats RunTcpProxyRelayBenchmark(const ProxyRelayOptions& options = {});
            // 格式化稳定的吞吐与 stats 两行输出。
            std::string FormatProxyRelayStats(const ProxyRelayStats& stats);
        }
    }
}
