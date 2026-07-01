#include <LikesProgram/Metrics/Metrics.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#define LP_BENCH_NOINLINE __declspec(noinline)
#else
#define LP_BENCH_NOINLINE __attribute__((noinline))
#endif

namespace {
    std::atomic<std::uint64_t> g_probe{ 0 }; // 防止编译器把基准循环整体消除

    // 给每个基准循环提供可观察副作用，保持被测路径真实。
    std::uint64_t Probe() {
        return g_probe.load(std::memory_order_relaxed);
    }

    template<typename F>
    long long MeasureNs(F&& fn) {
        auto begin = std::chrono::steady_clock::now(); // 测量起点
        volatile std::uint64_t sink = fn();            // 保存结果避免优化消除
        (void)sink;
        auto end = std::chrono::steady_clock::now();   // 测量终点
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }

    // 统一输出 LikesProgram 路径和 std 对照路径，便于趋势记录。
    void Print(const char* name, long long likesNs, long long stdNs) {
        std::cout << name
            << " likes_ns=" << likesNs
            << " std_ns=" << stdNs
            << std::endl;
    }

    // std 原子 double 饱和加法对照，语义接近 Metrics 的长期运行保护。
    LP_BENCH_NOINLINE void StdAddSaturating(std::atomic<double>& target, double value) {
        double current = target.load(std::memory_order_relaxed); // CAS 当前快照
        while (!target.compare_exchange_weak(current, current + value,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    void BenchmarkCounter() {
        constexpr int iterations = 300000; // Counter 单线程热路径次数
        LikesProgram::Metrics::Counter counter(u"benchmark_counter_total", u"Benchmark counter");

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0; // 循环结果累积，防止优化
            for (int i = 0; i < iterations; ++i) {
                counter.Increment();
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(counter.Value());
        });

        std::atomic<double> stdCounter{ 0.0 }; // std 对照原子计数
        auto std = MeasureNs([&] {
            std::uint64_t total = 0; // std 路径可观察结果
            for (int i = 0; i < iterations; ++i) {
                StdAddSaturating(stdCounter, 1.0);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(stdCounter.load(std::memory_order_relaxed));
        });

        Print("counter_increment_vs_std_atomic", likes, std);
    }

    void BenchmarkGauge() {
        constexpr int iterations = 300000; // Gauge 正负变化热路径次数
        LikesProgram::Metrics::Gauge gauge(u"benchmark_gauge", u"Benchmark gauge");

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0; // 可观察累积
            for (int i = 0; i < iterations; ++i) {
                gauge.Increment(1.0);
                gauge.Decrement(1.0);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(gauge.Value());
        });

        std::atomic<double> stdGauge{ 0.0 }; // std 对照瞬时值
        auto std = MeasureNs([&] {
            std::uint64_t total = 0; // std 路径可观察累积
            for (int i = 0; i < iterations; ++i) {
                StdAddSaturating(stdGauge, 1.0);
                StdAddSaturating(stdGauge, -1.0);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(stdGauge.load(std::memory_order_relaxed));
        });

        Print("gauge_inc_dec_vs_std_atomic", likes, std);
    }

    void BenchmarkHistogram() {
        constexpr int iterations = 120000; // Histogram 多桶采样次数
        std::vector<double> buckets;       // 100 个固定桶，对照多桶服务场景
        for (int i = 1; i <= 100; ++i) buckets.push_back(static_cast<double>(i) / 10.0);

        LikesProgram::Metrics::Histogram histogram(
            u"benchmark_histogram",
            buckets,
            u"Benchmark histogram");

        auto likes = MeasureNs([&] {
            std::uint64_t total = 0; // 采样循环可观察累积
            for (int i = 0; i < iterations; ++i) {
                histogram.Observe(static_cast<double>(i % 1000) / 100.0);
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(histogram.Count());
        });

        std::vector<std::int64_t> stdCounts(buckets.size(), 0); // std 对照命中桶计数
        std::int64_t stdTotal = 0;                              // std 对照总样本数
        auto std = MeasureNs([&] {
            std::uint64_t total = 0; // std 路径可观察累积
            for (int i = 0; i < iterations; ++i) {
                const double value = static_cast<double>(i % 1000) / 100.0;
                auto iter = std::lower_bound(buckets.begin(), buckets.end(), value);
                if (iter != buckets.end()) {
                    ++stdCounts[static_cast<size_t>(iter - buckets.begin())];
                }
                ++stdTotal;
                total += static_cast<std::uint64_t>(i) + Probe();
            }
            return total + static_cast<std::uint64_t>(stdTotal);
        });

        Print("histogram_observe_100_buckets_vs_std_lower_bound", likes, std);
    }

    void BenchmarkSummaryMultiThread() {
        constexpr int threadCount = 4;  // 多线程采样线程数
        constexpr int perThread = 20000;
        LikesProgram::Metrics::Summary summary(u"benchmark_summary", 512, u"Benchmark summary");

        auto likes = MeasureNs([&] {
            std::vector<std::thread> threads; // Summary 多生产者写入线程
            for (int t = 0; t < threadCount; ++t) {
                threads.emplace_back([&summary, t] {
                    for (int i = 0; i < perThread; ++i) {
                        summary.Observe(static_cast<double>((i + t) % 1000) / 1000.0);
                    }
                });
            }
            for (auto& thread : threads) thread.join();

            const double p50 = summary.Quantile(0.50); // 查询端到端分位数成本
            const double p90 = summary.Quantile(0.90);
            const double p99 = summary.Quantile(0.99);
            return static_cast<std::uint64_t>(summary.Count())
                + static_cast<std::uint64_t>((p50 + p90 + p99) * 1000000.0)
                + Probe();
        });

        std::mutex mutex;                       // std 对照保护样本数组
        std::vector<double> values;             // std 对照收集所有样本
        values.reserve(threadCount * perThread);
        auto std = MeasureNs([&] {
            std::vector<std::thread> threads; // std 多生产者写入线程
            for (int t = 0; t < threadCount; ++t) {
                threads.emplace_back([&values, &mutex, t] {
                    for (int i = 0; i < perThread; ++i) {
                        std::lock_guard<std::mutex> lock(mutex);
                        values.push_back(static_cast<double>((i + t) % 1000) / 1000.0);
                    }
                });
            }
            for (auto& thread : threads) thread.join();

            std::sort(values.begin(), values.end()); // exact 分位数需要完整排序
            auto quantile = [&values](double q) {
                if (values.empty()) return 0.0;
                const auto rank = static_cast<size_t>(std::ceil(q * static_cast<double>(values.size())));
                const size_t index = rank == 0 ? 0 : std::min(rank - 1, values.size() - 1);
                return values[index];
            };
            const double p50 = quantile(0.50); // 与 Summary 路径保持端到端口径
            const double p90 = quantile(0.90);
            const double p99 = quantile(0.99);
            return static_cast<std::uint64_t>(values.size())
                + static_cast<std::uint64_t>((p50 + p90 + p99) * 1000000.0)
                + Probe();
        });

        Print("summary_multithread_observe_vs_std_mutex_vector", likes, std);
    }

    void BenchmarkRegistryExport() {
        constexpr int metricCount = 200; // 注册表规模覆盖普通服务指标数量级
        LikesProgram::Metrics::Registry registry;
        for (int i = 0; i < metricCount; ++i) {
            auto counter = std::make_shared<LikesProgram::Metrics::Counter>(
                LikesProgram::String::Format(u"benchmark_metric_{}_total", i),
                u"Benchmark metric",
                std::map<LikesProgram::String, LikesProgram::String>{
                    { u"slot", LikesProgram::String::Format(u"{}", i) } });
            counter->Increment(static_cast<double>(i));
            registry.Register(counter);
        }

        auto likes = MeasureNs([&] {
            auto text = registry.ExportPrometheus(); // 完整 Prometheus 导出路径
            return static_cast<std::uint64_t>(text.Length()) + Probe();
        });

        std::vector<std::string> stdLines; // std 对照简单字符串拼接
        stdLines.reserve(metricCount);
        auto std = MeasureNs([&] {
            std::string text;
            for (int i = 0; i < metricCount; ++i) {
                text += "benchmark_metric_" + std::to_string(i)
                    + "_total{slot=\"" + std::to_string(i) + "\"} "
                    + std::to_string(i) + "\n";
            }
            return static_cast<std::uint64_t>(text.size()) + Probe();
        });

        Print("registry_export_prometheus_200_vs_std_string_build", likes, std);
    }
}

// Metrics Release benchmark 入口，输出格式和其他包保持一致。
int main() {
    BenchmarkCounter();
    BenchmarkGauge();
    BenchmarkHistogram();
    BenchmarkSummaryMultiThread();
    BenchmarkRegistryExport();
    return 0;
}
