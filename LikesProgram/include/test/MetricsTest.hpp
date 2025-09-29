#pragma once
#include "../LikesProgram/Metrics/Counter.hpp"
#include "../LikesProgram/Metrics/Gauge.hpp"
#include "../LikesProgram/Metrics/Histogram.hpp"
#include "../LikesProgram/Metrics/Summary.hpp"
#include "../LikesProgram/Metrics/Registry.hpp"
#include "../LikesProgram/time/Timer.hpp"
#include <iostream>
#include <thread>
#include <chrono>

namespace MetricsTest {
    void Test() {
        std::cout << "\n===== Counter 示例 =====\n" << std::endl;
        auto httpCounter = std::make_shared<LikesProgram::Metrics::Counter>(
            u"http_requests_total",
            u"Total HTTP requests",
            std::map<LikesProgram::String, LikesProgram::String>{{u"method", u"GET"}, { u"code", u"200" }}
        );

        httpCounter->Increment();       // 默认 +1
        httpCounter->Increment(5.0);    // +5

        std::wcout << L"Counter Value: " << httpCounter->Value() << std::endl;
        std::wcout << L"Prometheus Counter:\n" << httpCounter->ToPrometheus() << std::endl;
        std::wcout << L"Json Counter:\n" << httpCounter->ToJson() << std::endl << std::endl;


        std::cout << "\n===== Gauge 示例 =====\n" << std::endl;
        auto tempGauge = std::make_shared<LikesProgram::Metrics::Gauge>(
            u"temperature_celsius",
            u"Temperature in Celsius",
            std::map<LikesProgram::String, LikesProgram::String>{{u"room", u"server"}}
        );

        tempGauge->Set(25.0);
        tempGauge->Increment(3.0);
        tempGauge->Decrement(2.0);

        std::wcout << L"Gauge Value: " << tempGauge->Value() << std::endl;
        std::wcout << L"Prometheus Gauge:\n" << tempGauge->ToPrometheus() << std::endl;
        std::wcout << L"Json Gauge:\n" << tempGauge->ToJson() << std::endl << std::endl;


        std::cout << "\n===== Histogram 示例 =====\n" << std::endl;
        auto hist = std::make_shared<LikesProgram::Metrics::Histogram>(
            u"db_query_duration_seconds",
            std::vector<double>{0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0}, // 桶边界: 1ms ~ 1s
            u"Database query latency",
            std::map<LikesProgram::String, LikesProgram::String>{{u"db", u"users"}}
        );

        // 模拟三次数据库查询
        {
            LikesProgram::Time::Timer timer(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            timer.Stop();
            hist->ObserveDuration(timer); // ~0.002s
        }

        {
            LikesProgram::Time::Timer timer(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            timer.Stop();
            hist->ObserveDuration(timer); // ~0.03s
        }

        {
            LikesProgram::Time::Timer timer(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            timer.Stop();
            hist->ObserveDuration(timer); // ~0.2s
        }

        // 额外记录一次手动值
        hist->Observe(0.8); // 800ms

        // 导出结果
        std::wcout << L"Prometheus Histogram:\n" << hist->ToPrometheus() << std::endl;
        std::wcout << L"Json Histogram:\n" << hist->ToJson() << std::endl << std::endl;


        std::cout << "\n===== Summary 示例 =====\n" << std::endl;
        auto latencySummary = std::make_shared<LikesProgram::Metrics::Summary>(
            u"http_request_latency_seconds",
            1000,
            u"HTTP request latency summary",
            std::map<LikesProgram::String, LikesProgram::String>{{u"endpoint", u"/api"}}
        );

        latencySummary->Observe(0.05);
        latencySummary->Observe(0.10);
        latencySummary->Observe(0.20);

        std::wcout << L"Summary p95: " << latencySummary->Quantile(0.95) << std::endl;
        std::wcout << L"Prometheus Summary:\n" << latencySummary->ToPrometheus() << std::endl;
        std::wcout << L"Json Summary:\n" << latencySummary->ToJson() << std::endl << std::endl;


        std::cout << "\n===== Registry 示例 =====\n" << std::endl;
        auto& reg = LikesProgram::Metrics::Registry::Global();

        // Register
        reg.Register(httpCounter);
        reg.Register(tempGauge);
        reg.Register(hist);
        reg.Register(latencySummary);

        // GetMetrics
        auto got = reg.GetMetrics(u"http_requests_total", { {u"method", u"GET"}, {u"code", u"200"} });
        if (got) {
            std::wcout << L"Got metric: " << got->Name() << std::endl;
        }

        // ExportPrometheus
        LikesProgram::String prometheusOutput = reg.ExportPrometheus();
        std::wcout << L"Prometheus Export:\n" << prometheusOutput << std::endl;

        // ExportJson
        LikesProgram::String jsonOutput = reg.ExportJson();
        std::wcout << L"JSON Export:\n" << jsonOutput << std::endl;

        // Unregister
        reg.Unregister(u"http_requests_total", { {u"method", u"GET"}, {u"code", u"200"} });

        // 再导出看看是否移除了
        LikesProgram::String afterUnreg = reg.ExportPrometheus();
        std::wcout << L"After Unregister:\n" << afterUnreg << std::endl;
    }

    // 快速测试入口
    inline void TestQuick() {
        // 默认参数
        std::string mode = "leak";
        size_t iters = 100000;
        size_t alloc_bytes = 64; // 每次泄漏分配大小
        size_t report_interval = 10000;

        std::wcout << L"Mode: " << std::wstring(mode.begin(), mode.end())
            << L", iters=" << iters
            << L", alloc=" << alloc_bytes
            << L", report_interval=" << report_interval << std::endl;

        // 创建一组 metrics 示例（保留原示例的部分）
        auto httpCounter = std::make_shared<LikesProgram::Metrics::Counter>(
            u"http_requests_total",
            u"Total HTTP requests",
            std::map<LikesProgram::String, LikesProgram::String>{{u"method", u"GET"}, { u"code", u"200" }}
        );

        httpCounter->Increment();
        httpCounter->Increment(2.0);

        auto tempGauge = std::make_shared<LikesProgram::Metrics::Gauge>(
            u"temperature_celsius",
            u"Temperature in Celsius",
            std::map<LikesProgram::String, LikesProgram::String>{{u"room", u"server"}}
        );

        tempGauge->Set(25.0);
        tempGauge->Increment(3.0);
        tempGauge->Decrement(2.0);

        auto hist = std::make_shared<LikesProgram::Metrics::Histogram>(
            u"db_query_duration_seconds",
            std::vector<double>{0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0},
            u"Database query latency",
            std::map<LikesProgram::String, LikesProgram::String>{{u"db", u"users"}}
        );

        hist->Observe(0.8); // 800ms

        auto latencySummary = std::make_shared<LikesProgram::Metrics::Summary>(
            u"http_request_latency_seconds",
            1000,
            u"HTTP request latency summary",
            std::map<LikesProgram::String, LikesProgram::String>{{u"endpoint", u"/api"}}
        );

        latencySummary->Observe(0.05);
        latencySummary->Observe(0.10);
        latencySummary->Observe(0.20);

        // 注册到 Registry（保持行为）
        auto& reg = LikesProgram::Metrics::Registry::Global();
        reg.Register(httpCounter);
        reg.Register(tempGauge);
        reg.Register(hist);
        reg.Register(latencySummary);

        // ExportPrometheus
        LikesProgram::String prometheusOutput = reg.ExportPrometheus();
        std::wcout << L"Prometheus Export:\n" << prometheusOutput << std::endl;
    }
}
