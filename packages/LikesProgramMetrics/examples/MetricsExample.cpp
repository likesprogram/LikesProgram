#include <LikesProgram/Metrics/Metrics.hpp>

#include <iostream>
#include <memory>

// Metrics 示例展示计数、瞬时值、分布统计和注册表统一导出。
int main() {
    LikesProgram::Metrics::Registry registry;

    auto requests = std::make_shared<LikesProgram::Metrics::Counter>(
        u"http_requests_total",
        u"Total HTTP requests",
        std::map<LikesProgram::String, LikesProgram::String>{ { u"method", u"GET" } });
    requests->Increment(3.0);

    auto workers = std::make_shared<LikesProgram::Metrics::Gauge>(
        u"worker_threads",
        u"Current worker threads");
    workers->Set(8.0);

    auto latency = std::make_shared<LikesProgram::Metrics::Histogram>(
        u"http_request_duration_seconds",
        std::vector<double>{ 0.05, 0.1, 0.5, 1.0 },
        u"HTTP request duration",
        std::map<LikesProgram::String, LikesProgram::String>{ { u"route", u"/api" } });
    latency->Observe(0.07);
    latency->Observe(0.3);

    auto summary = std::make_shared<LikesProgram::Metrics::Summary>(
        u"http_request_latency_summary",
        128,
        u"HTTP request latency summary");
    summary->Observe(0.07);
    summary->Observe(0.3);

    registry.Register(requests);
    registry.Register(workers);
    registry.Register(latency);
    registry.Register(summary);

    std::cout << LikesProgram::Metrics::PackageName() << " "
        << LikesProgram::Metrics::PackageVersion() << std::endl;
    std::cout << registry.ExportPrometheus().ToStdString() << std::endl;
    return LikesProgram::Metrics::PackageAvailable() ? 0 : 1;
}
