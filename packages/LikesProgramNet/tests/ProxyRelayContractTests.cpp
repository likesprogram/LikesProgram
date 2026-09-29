#include "ProxyRelayBenchmark.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
    using LikesProgram::Net::Benchmarks::FormatProxyRelayStats;
    using LikesProgram::Net::Benchmarks::ProxyRelayOptions;
    using LikesProgram::Net::Benchmarks::ProxyRelayScenario;
    using LikesProgram::Net::Benchmarks::ProxyRelayStats;
    using LikesProgram::Net::Benchmarks::RunTcpProxyRelayBenchmark;

    bool Fail(const char* message) {
        std::cerr << message << '\n';
        return false;
    }

    ProxyRelayOptions DirectedWatermarkOptions() {
        ProxyRelayOptions options; // 两侧使用独立阈值，稳定触发 high/low 回调
        options.payloadBytes = 64 * 1024;
        options.ingressWriteHighWatermarkBytes = 1024;
        options.ingressWriteLowWatermarkBytes = 0;
        options.upstreamWriteHighWatermarkBytes = 1024;
        options.upstreamWriteLowWatermarkBytes = 0;
        return options;
    }

    bool CheckFullDuplexStats(
        const ProxyRelayStats& stats,
        const ProxyRelayOptions& options,
        bool requireEarlyQueue) {
        return stats.clientReceivedBytes == options.payloadBytes
            && stats.backendReceivedBytes == options.payloadBytes
            && stats.ingressToUpstreamBytes == options.payloadBytes
            && stats.upstreamToIngressBytes == options.payloadBytes
            && stats.fullDuplexOverlapObserved
            && (!requireEarlyQueue || stats.earlyQueuePeakBytes != 0)
            && stats.earlyQueuePeakBytes <= options.earlyQueueLimitBytes
            && stats.ingressHighWatermarkEvents > 0
            && stats.ingressLowWatermarkEvents > 0
            && stats.upstreamHighWatermarkEvents > 0
            && stats.upstreamLowWatermarkEvents > 0
            && !stats.closeReason.empty()
            && stats.resourcesConverged
            && stats.cleanShutdown
            && stats.upstreamClientOwnerStopEvents == 1
            && stats.upstreamClientWrongThreadStops == 0
            && stats.errors == 0;
    }

    void PrintRelayStats(const ProxyRelayStats& stats) {
        // 失败时保留完整公开指标与内部生命周期标志，便于定位偶发计数缺口。
        std::cerr << FormatProxyRelayStats(stats)
            << "full_duplex_overlap_observed=" << (stats.fullDuplexOverlapObserved ? 1 : 0)
            << " resources_converged=" << (stats.resourcesConverged ? 1 : 0)
            << " owner_stop_attempts=" << stats.ownerStopAttempts
            << " owner_stop_events=" << stats.upstreamClientOwnerStopEvents
            << " wrong_thread_stops=" << stats.upstreamClientWrongThreadStops
            << '\n';
    }

    bool TestFullDuplexAndWatermarks(ProxyRelayStats& formattedStats) {
        auto options = DirectedWatermarkOptions(); // 延迟 upstream，稳定形成早到队列与双向同时在途
        options.upstreamStartDelay = std::chrono::milliseconds(250);
        formattedStats = RunTcpProxyRelayBenchmark(options);
        if (!CheckFullDuplexStats(formattedStats, options, true)) {
            PrintRelayStats(formattedStats);
            return Fail("proxy relay full duplex contract mismatch");
        }

        options.payloadBytes = 32 * 1024; // 无延迟路径不得依赖早到队列
        options.upstreamStartDelay = std::chrono::milliseconds::zero();
        const auto immediateStats = RunTcpProxyRelayBenchmark(options);
        if (!CheckFullDuplexStats(immediateStats, options, false)) {
            PrintRelayStats(immediateStats);
            return Fail("proxy relay immediate shutdown contract mismatch");
        }
        return true;
    }

    bool TestForceClosePaths() {
        ProxyRelayOptions options;
        options.payloadBytes = 8 * 1024;
        options.upstreamStartDelay = std::chrono::milliseconds(50);
        options.forceCloseBeforeUpstream = true;
        const auto forceCloseStats = RunTcpProxyRelayBenchmark(options);
        if (!forceCloseStats.cleanShutdown
            || !forceCloseStats.resourcesConverged
            || forceCloseStats.closeReason.empty()
            || forceCloseStats.errors != 0) {
            return Fail("proxy relay force close contract mismatch");
        }

        options.forceCloseBeforeUpstream = false; // worker 已锁 session 后再关 ingress
        options.forceCloseAfterDelayWorkerLock = true;
        options.delayWorkerHoldAfterLock = std::chrono::milliseconds(100);
        const auto lockedWorkerStats = RunTcpProxyRelayBenchmark(options);
        if (!lockedWorkerStats.delayWorkerLocked
            || !lockedWorkerStats.delayWorkerExited
            || lockedWorkerStats.delayWorkerJoinEvents != 1
            || !lockedWorkerStats.resourcesConverged
            || !lockedWorkerStats.cleanShutdown) {
            return Fail("proxy relay locked worker contract mismatch");
        }
        return true;
    }

    bool TestConcurrentOwnerClose() {
        ProxyRelayOptions options;
        options.payloadBytes = 8 * 1024;
        options.scenario = ProxyRelayScenario::ConcurrentClose;
        options.holdRemoteStopBeforePost = true;
        const auto stats = RunTcpProxyRelayBenchmark(options);
        if (stats.ownerStopAttempts == 0
            || stats.upstreamClientOwnerStopEvents != 1
            || stats.upstreamClientWrongThreadStops != 0
            || !stats.resourcesConverged
            || !stats.cleanShutdown) {
            return Fail("proxy relay concurrent close owner contract mismatch");
        }
        return true;
    }

    bool TestExpiredHandoffDrop() {
        ProxyRelayOptions options;
        options.payloadBytes = 8 * 1024;
        options.scenario = ProxyRelayScenario::HandoffClose;
        options.holdUpstreamBeforeOwnerQueue = true;
        const auto stats = RunTcpProxyRelayBenchmark(options);
        if (stats.expiredHandoffDrops != 1
            || stats.upstreamClientOwnerStopEvents != 1
            || stats.upstreamClientWrongThreadStops != 0
            || !stats.resourcesConverged
            || !stats.cleanShutdown) {
            return Fail("proxy relay expired handoff contract mismatch");
        }
        return true;
    }

    bool TestUpstreamEofOwnerStop() {
        ProxyRelayOptions options;
        options.payloadBytes = 8 * 1024;
        options.scenario = ProxyRelayScenario::UpstreamEof;
        const auto stats = RunTcpProxyRelayBenchmark(options);
        if (stats.upstreamClientOwnerStopEvents != 1
            || stats.upstreamClientWrongThreadStops != 0
            || stats.closeReason != "upstream_eof"
            || !stats.resourcesConverged) {
            return Fail("proxy relay upstream owner contract mismatch");
        }
        return true;
    }

    bool TestOverflowPaths() {
        ProxyRelayOptions earlyOptions;
        earlyOptions.scenario = ProxyRelayScenario::EarlyQueueOverflow;
        earlyOptions.payloadBytes = 32 * 1024;
        earlyOptions.earlyQueueLimitBytes = 8 * 1024;
        earlyOptions.upstreamStartDelay = std::chrono::milliseconds(250);
        const auto earlyStats = RunTcpProxyRelayBenchmark(earlyOptions);
        if (earlyStats.overflowEvents != 1
            || earlyStats.ingressToUpstreamBytes != 0
            || earlyStats.backendReceivedBytes != 0
            || earlyStats.closeReason != "early_queue_overflow"
            || !earlyStats.resourcesConverged) {
            PrintRelayStats(earlyStats);
            return Fail("proxy relay early overflow contract mismatch");
        }

        ProxyRelayOptions writeOptions; // 单次转发超过 upstream 写硬上限
        writeOptions.scenario = ProxyRelayScenario::UpstreamWriteOverflow;
        writeOptions.payloadBytes = 32 * 1024;
        writeOptions.earlyQueueLimitBytes = 64 * 1024;
        writeOptions.upstreamStartDelay = std::chrono::milliseconds(50);
        writeOptions.upstreamMaxPendingWriteBytes = 8 * 1024;
        const auto writeStats = RunTcpProxyRelayBenchmark(writeOptions);
        if (writeStats.overflowEvents != 1
            || writeStats.ingressToUpstreamBytes != writeStats.backendReceivedBytes
            || writeStats.ingressToUpstreamBytes >= writeOptions.payloadBytes
            || writeStats.closeReason != "upstream_write_overflow"
            || !writeStats.resourcesConverged) {
            PrintRelayStats(writeStats);
            return Fail("proxy relay write overflow contract mismatch");
        }
        return true;
    }

    bool TestActiveShutdownAndConnectFailure() {
        ProxyRelayOptions shutdownOptions;
        shutdownOptions.payloadBytes = 32 * 1024;
        shutdownOptions.scenario = ProxyRelayScenario::RelayShutdown;
        const auto shutdownStats = RunTcpProxyRelayBenchmark(shutdownOptions);
        if (!shutdownStats.resourcesConverged
            || shutdownStats.upstreamClientOwnerStopEvents != 1
            || shutdownStats.upstreamClientWrongThreadStops != 0
            || shutdownStats.closeReason != "relay_shutdown") {
            return Fail("proxy relay active shutdown contract mismatch");
        }

        ProxyRelayOptions failureOptions;
        failureOptions.payloadBytes = 8 * 1024;
        failureOptions.scenario = ProxyRelayScenario::ConnectFailure;
        const auto failureStats = RunTcpProxyRelayBenchmark(failureOptions);
        if (failureStats.errors != 1
            || failureStats.closeReason != "upstream_connect_failure"
            || !failureStats.resourcesConverged) {
            return Fail("proxy relay connect failure contract mismatch");
        }
        return true;
    }

    bool TestStableFormat(const ProxyRelayStats& stats) {
        const std::string formatted = FormatProxyRelayStats(stats);
        const char* requiredFields[] = {
            "tcp_proxy_full_duplex_bytes_per_second seconds=",
            " throughput=",
            " unit=bytes/s",
            "tcp_proxy_full_duplex_stats ingress_to_upstream_bytes=",
            " upstream_to_ingress_bytes=",
            " backend_received_bytes=",
            " client_received_bytes=",
            " early_queue_peak_bytes=",
            " early_queue_limit_bytes=",
            " ingress_high_watermark_events=",
            " ingress_low_watermark_events=",
            " upstream_high_watermark_events=",
            " upstream_low_watermark_events=",
            " overflow_events=",
            " clean_shutdown=",
            " close_reason=",
            " errors="
        };
        for (const char* field : requiredFields) {
            if (formatted.find(field) == std::string::npos) {
                std::cerr << "proxy relay format contract mismatch: " << field << '\n';
                return false;
            }
        }
        return true;
    }
}

int main() {
    try {
        ProxyRelayStats formattedStats; // formatter 使用已通过真实 full-duplex 的快照
        const bool passed = TestFullDuplexAndWatermarks(formattedStats)
            && TestForceClosePaths()
            && TestUpstreamEofOwnerStop()
            && TestConcurrentOwnerClose()
            && TestExpiredHandoffDrop()
            && TestOverflowPaths()
            && TestActiveShutdownAndConnectFailure()
            && TestStableFormat(formattedStats);
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch (const std::exception& ex) {
        std::cerr << "proxy relay contract exception: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
