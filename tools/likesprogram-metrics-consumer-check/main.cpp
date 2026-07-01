#include <LikesProgram/Metrics/Metrics.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <string>

int main() {
    if (!LikesProgram::Metrics::PackageAvailable()) return 1;

    LikesProgram::Metrics::Registry registry; // 外部消费方独立指标注册表
    auto counter = std::make_shared<LikesProgram::Metrics::Counter>(
        u"consumer_requests_total",
        u"Consumer requests",
        std::map<LikesProgram::String, LikesProgram::String>{ { u"component", u"metrics" } });

    counter->Increment(2.0);
    registry.Register(counter);
    if (registry.Count() != 1) return 2;

    const std::string prometheus = registry.ExportPrometheus().ToStdString(); // 验证导出主路径
    if (prometheus.find("consumer_requests_total{component=\"metrics\"} 2.000000")
        == std::string::npos) {
        return 3;
    }

    std::cout << LikesProgram::Metrics::PackageName()
        << " consumer check passed\n";
    return 0;
}
