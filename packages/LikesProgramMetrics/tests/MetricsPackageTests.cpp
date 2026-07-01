#include <LikesProgram/Metrics/Metrics.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <metrics/PercentileSketch.hpp>

#include <cmath>
#include <cstring>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    // Metrics 包回归测试覆盖指标采样、导出、注册表和包身份。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 浮点比较只用于本包测试的小数值，误差阈值保持简单明确。
    bool Near(double left, double right, double epsilon = 1e-9) {
        return std::fabs(left - right) <= epsilon;
    }

    // 判断 UTF-8 导出文本是否包含目标片段。
    bool Contains(const LikesProgram::String& text, const std::string& needle) {
        return text.ToStdString().find(needle) != std::string::npos;
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Metrics::PackageName(); // Metrics 包名指针
        const char* packageVersion = LikesProgram::Metrics::PackageVersion(); // Metrics 版本指针

        Require(LikesProgram::Metrics::PackageAvailable(), "Metrics package should be available");
        Require(std::strcmp(packageName, "LikesProgramMetrics") == 0, "Metrics package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Metrics package version should follow Core version");
    }

    void TestCounter() {
        LikesProgram::Metrics::Counter counter(
            u"http_requests_total",
            u"Total HTTP requests",
            { { u"method", u"GET" }, { u"code", u"200" } });

        counter.Increment();
        counter.Increment(4.0);
        counter.Increment(-10.0);

        Require(Near(counter.Value(), 5.0), "Counter should ignore negative increments");
        Require(Contains(counter.ToPrometheus(), "http_requests_total{code=\"200\",method=\"GET\"} 5.000000"),
            "Counter Prometheus export should contain value and labels");
        Require(Contains(counter.ToJson(), "\"value\":5.000000"),
            "Counter JSON export should contain value field");

        counter.Reset();
        Require(Near(counter.Value(), 0.0), "Counter Reset should clear value");

        counter.Increment(std::numeric_limits<double>::max());
        counter.Increment(std::numeric_limits<double>::max());
        Require(std::isfinite(counter.Value()), "Counter should saturate overflow to finite value");
    }

    void TestGauge() {
        LikesProgram::Metrics::Gauge gauge(
            u"temperature_celsius",
            u"Temperature",
            { { u"room", u"server" } });

        gauge.Set(25.0);
        gauge.Increment(3.0);
        gauge.Decrement(2.0);

        Require(Near(gauge.Value(), 26.0), "Gauge arithmetic mismatch");
        Require(Contains(gauge.ToJson(), "\"value\":26.000000"),
            "Gauge JSON export should contain value field");
        Require(Contains(gauge.ToPrometheus(), "temperature_celsius{room=\"server\"} 26.000000"),
            "Gauge Prometheus export should contain value and labels");

        gauge.Set(std::numeric_limits<double>::max());
        gauge.Increment(std::numeric_limits<double>::max());
        Require(std::isfinite(gauge.Value()), "Gauge positive overflow should stay finite");
    }

    void TestHistogram() {
        LikesProgram::Metrics::Histogram histogram(
            u"request_duration_seconds",
            { 1.0, 0.1, 0.5, 0.5 },
            u"Request duration",
            { { u"route", u"/api" } });

        histogram.Observe(0.05);
        histogram.Observe(0.3);
        histogram.Observe(2.0);

        const auto buckets = histogram.Buckets(); // 规范化后的桶边界
        const auto counts = histogram.Counts();   // 桶累计计数快照

        Require(buckets.size() == 3, "Histogram should sort and deduplicate buckets");
        Require(Near(buckets[0], 0.1) && Near(buckets[1], 0.5) && Near(buckets[2], 1.0),
            "Histogram bucket order mismatch");
        Require(counts.size() == 3 && counts[0] == 1 && counts[1] == 2 && counts[2] == 2,
            "Histogram cumulative bucket counts mismatch");
        Require(histogram.Count() == 3, "Histogram total count mismatch");
        Require(Near(histogram.Sum(), 2.35), "Histogram sum mismatch");
        Require(Contains(histogram.ToPrometheus(),
            "request_duration_seconds_bucket{le=\"0.500000\",route=\"/api\"} 2"),
            "Histogram Prometheus export should include user labels and le");

        histogram.Observe(std::numeric_limits<double>::max());
        histogram.Observe(std::numeric_limits<double>::max());
        Require(std::isfinite(histogram.Sum()), "Histogram sum should stay finite after overflow pressure");
    }

    void TestSummary() {
        LikesProgram::Metrics::Summary summary(
            u"request_latency_seconds",
            128,
            u"Request latency",
            { { u"route", u"/api" } });

        summary.SetEMAAlpha(0.5);
        for (double value : { 0.1, 0.2, 0.3, 0.4, 0.5 }) {
            summary.Observe(value);
        }

        Require(summary.Count() == 5, "Summary count mismatch");
        Require(Near(summary.Sum(), 1.5), "Summary sum mismatch");
        Require(summary.Min() <= 0.1 && summary.Max() >= 0.5, "Summary min/max mismatch");
        Require(summary.Quantile(0.5) >= 0.1 && summary.Quantile(0.5) <= 0.5,
            "Summary median should stay within observed range");
        Require(Contains(summary.ToPrometheus(), "quantile=\"0.90\""),
            "Summary Prometheus export should include quantile label");
        Require(Contains(summary.ToJson(), "\"ema\":"),
            "Summary JSON export should include EMA when alpha is enabled");

        summary.Observe(std::numeric_limits<double>::max());
        summary.Observe(std::numeric_limits<double>::max());
        Require(std::isfinite(summary.Sum()), "Summary sum should stay finite after overflow pressure");
    }

    void TestConcurrentSampling() {
        constexpr int threadCount = 4;         // 并发写入线程数量
        constexpr int samplesPerThread = 2000; // 每线程样本数，覆盖轻量压力场景
        LikesProgram::Metrics::Counter counter(u"concurrent_counter_total", u"Concurrent counter");
        LikesProgram::Metrics::Gauge gauge(u"concurrent_gauge", u"Concurrent gauge");
        LikesProgram::Metrics::Histogram histogram(
            u"concurrent_histogram",
            { 0.1, 0.5, 1.0, 5.0 },
            u"Concurrent histogram");
        LikesProgram::Metrics::Summary summary(u"concurrent_summary", 256, u"Concurrent summary");

        std::vector<std::thread> threads; // 写入线程集合
        for (int t = 0; t < threadCount; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < samplesPerThread; ++i) {
                    const double value = static_cast<double>((i + t) % 100) / 100.0; // 稳定样本分布
                    counter.Increment();
                    gauge.Increment(1.0);
                    gauge.Decrement(1.0);
                    histogram.Observe(value);
                    summary.Observe(value);
                }
            });
        }
        for (auto& thread : threads) thread.join();

        const auto expected = static_cast<int64_t>(threadCount * samplesPerThread); // 期望总采样数
        Require(counter.Value() == static_cast<double>(expected), "Concurrent counter count mismatch");
        Require(histogram.Count() == expected, "Concurrent histogram count mismatch");
        Require(summary.Count() == expected, "Concurrent summary count mismatch");
        Require(std::isfinite(summary.Quantile(0.9)), "Concurrent summary quantile should stay finite");
        Require(Contains(histogram.ToPrometheus(), "concurrent_histogram_bucket"),
            "Concurrent histogram export should stay available");
    }

    void TestConcurrentRegistryExport() {
        LikesProgram::Metrics::Registry registry;
        auto counter = std::make_shared<LikesProgram::Metrics::Counter>(
            u"registry_concurrent_total",
            u"Registry concurrent counter");
        registry.Register(counter);

        std::atomic<bool> stop{ false };     // 导出线程停止标志
        std::atomic<int> exportCount{ 0 };   // 成功导出次数
        std::thread exporter([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto text = registry.ExportPrometheus(); // 无锁外部导出快照
                if (Contains(text, "registry_concurrent_total")) {
                    exportCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        for (int i = 0; i < 5000; ++i) {
            counter->Increment();
            if (i % 250 == 0) {
                auto gauge = std::make_shared<LikesProgram::Metrics::Gauge>(
                    u"registry_dynamic_gauge",
                    u"Registry dynamic gauge",
                    std::map<LikesProgram::String, LikesProgram::String>{ { u"slot", LikesProgram::String::Format(u"{}", i) } });
                gauge->Set(static_cast<double>(i));
                registry.Register(gauge);
                registry.Unregister(u"registry_dynamic_gauge",
                    { { u"slot", LikesProgram::String::Format(u"{}", i - 250) } });
            }
        }

        stop.store(true, std::memory_order_relaxed);
        exporter.join();
        Require(exportCount.load(std::memory_order_relaxed) > 0,
            "Registry should support concurrent export while sampling/registering");
        Require(counter->Value() == 5000.0, "Registry concurrent counter mismatch");
    }

    void TestPercentileSketchMerge() {
        LikesProgram::Metrics::Internal::PercentileSketch left(64, 2);
        LikesProgram::Metrics::Internal::PercentileSketch right(64, 2);

        for (int i = 0; i < 80; ++i) {
            left.Add(static_cast<double>(i)); // 触发左侧 flush 后生成质心
        }
        for (int i = 80; i < 100; ++i) {
            right.Add(static_cast<double>(i)); // 右侧保持未 flush 缓冲，覆盖合并完整性
        }

        left.Merge(right);
        const double p99 = left.Quantile(0.99); // 合并后尾部分位应看到右侧高值
        Require(std::isfinite(p99), "Merged sketch quantile should be finite");
        Require(p99 >= 90.0, "Merged sketch should include buffered source samples");
    }

    void TestRegistry() {
        LikesProgram::Metrics::Registry registry;
        auto counter = std::make_shared<LikesProgram::Metrics::Counter>(
            u"jobs_total",
            u"Jobs",
            std::map<LikesProgram::String, LikesProgram::String>{ { u"queue", u"default" } });
        auto gauge = std::make_shared<LikesProgram::Metrics::Gauge>(u"workers", u"Workers");

        counter->Increment(2.0);
        gauge->Set(4.0);

        registry.Register(counter);
        registry.Register(gauge);
        Require(registry.Count() == 2, "Registry count mismatch after register");
        Require(registry.GetMetrics(u"jobs_total", { { u"queue", u"default" } }) == counter,
            "Registry should find registered counter by labels");

        auto replacement = std::make_shared<LikesProgram::Metrics::Counter>(
            u"jobs_total",
            u"Jobs v2",
            std::map<LikesProgram::String, LikesProgram::String>{ { u"queue", u"default" } });
        registry.Register(replacement);
        Require(registry.Count() == 2, "Registry should replace duplicate key");
        Require(registry.GetMetrics(u"jobs_total", { { u"queue", u"default" } }) == replacement,
            "Registry should return replacement metric");

        Require(Contains(registry.ExportJson(), "\"name\":\"jobs_total\""),
            "Registry JSON export should contain registered metric");
        registry.Unregister(u"jobs_total", { { u"queue", u"default" } });
        Require(registry.Count() == 1, "Registry count mismatch after unregister");

        registry.Clear();
        Require(registry.Count() == 0, "Registry Clear should remove all metrics");
    }
}

// Metrics 测试入口，失败时输出明确错误并返回非零。
int main() {
    try {
        TestPackageIdentity();
        TestCounter();
        TestGauge();
        TestHistogram();
        TestSummary();
        TestConcurrentSampling();
        TestPercentileSketchMerge();
        TestRegistry();
        TestConcurrentRegistryExport();

        std::cout << "LikesProgramMetricsTests passed" << std::endl;
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "LikesProgramMetricsTests failed: " << ex.what() << std::endl;
        return 1;
    }
}
