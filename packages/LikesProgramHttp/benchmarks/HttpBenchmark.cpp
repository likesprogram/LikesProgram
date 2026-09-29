#include <LikesProgram/Http/Http.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    volatile std::uint64_t g_benchmarkSink = 0; // 保存基准校验和，阻止整段循环被优化

    struct Measurement {
        long long totalNs = 0;               // 整轮耗时，单位纳秒
        std::uint64_t checksum = 0;           // 被测路径的可观察校验和
    };

    template<typename F>
    Measurement Measure(F&& fn) {
        const auto begin = std::chrono::steady_clock::now(); // 当前 workload 起点
        const std::uint64_t checksum = fn();                  // 执行完整固定轮次
        const auto end = std::chrono::steady_clock::now();   // 当前 workload 终点
        g_benchmarkSink = checksum;

        return Measurement{ std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count(), checksum };
    }

    void PrintMeasurement(const char* name,
        const char* reference,
        std::uint64_t iterations,
        std::uint64_t bytesPerIteration,
        const Measurement& measurement) {
        const double totalNs = static_cast<double>(measurement.totalNs); // 浮点输出基准
        const double nsPerOperation = totalNs / static_cast<double>(iterations);
        const double operationsPerSecond = totalNs <= 0.0 ? 0.0 : static_cast<double>(iterations) * 1000000000.0 / totalNs;
        const double mibPerSecond = totalNs <= 0.0 ? 0.0 : static_cast<double>(bytesPerIteration) * static_cast<double>(iterations) * 1000000000.0 / totalNs / (1024.0 * 1024.0);

        std::cout << std::fixed << std::setprecision(3)
            << "name=" << name
            << " reference=" << reference
            << " iterations=" << iterations
            << " bytes_per_iteration=" << bytesPerIteration
            << " total_ns=" << measurement.totalNs
            << " ns_per_operation=" << nsPerOperation
            << " operations_per_second=" << operationsPerSecond
            << " mib_per_second=" << mibPerSecond
            << " checksum=" << measurement.checksum
            << '\n';
    }

    template<typename T>
    const T& RequireValue(const LikesProgram::Result<T>& result, const char* message) {
        if (!result.IsOk()) throw std::runtime_error(message);
        return result.Value();
    }

    class BenchmarkTransport final : public LikesProgram::Http::HttpTransport {
    public:
        // 最小本地传输只返回固定响应，用于测量 Session 分发开销。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            LikesProgram::Http::HttpVersion version) override {
            LikesProgram::Http::HttpResponse response;
            response.statusCode = version == LikesProgram::Http::HttpVersion::Http3 ? 203 : 200;
            response.body.assign(request.target.begin(), request.target.end());
            return response;
        }
    };

    void BenchmarkHttp1() {
        constexpr std::uint64_t iterations = 50000; // 包含分配的完整组装+解析轮次
        LikesProgram::Http::HttpRequest request;
        request.method = "POST";
        request.target = "/benchmark/orders?id=42";
        request.headers = {
            { "Host", "example.test" },
            { "Content-Type", "application/octet-stream" },
            { "X-Request-Id", "likesprogram-http-benchmark" }
        };
        request.body.resize(256, 0x5A);

        const auto baselineBytes = RequireValue(LikesProgram::Http::BuildHttp1Request(request), "HTTP/1 benchmark baseline build failed");

        const auto roundTrip = Measure([&] {
            std::uint64_t checksum = 0; // 累积状态码、字段和正文长度
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const auto built = LikesProgram::Http::BuildHttp1Request(request);
                if (!built.IsOk()) throw std::runtime_error("HTTP/1 benchmark build failed");
                const auto parsed = LikesProgram::Http::ParseHttp1Request(built.Value());
                if (!parsed.IsOk()) throw std::runtime_error("HTTP/1 benchmark parse failed");
                checksum += parsed.Value().body.size() + parsed.Value().headers.size();
            }
            return checksum;
        });
        PrintMeasurement("http1_build_parse", "protocol_codec", iterations, baselineBytes.size(), roundTrip);

        const auto copyReference = Measure([&] {
            std::uint64_t checksum = 0; // std::string 复制只作为内存下界
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const std::string copy(baselineBytes);
                checksum += copy.size() + static_cast<unsigned char>(copy.front());
            }
            return checksum;
        });
        PrintMeasurement("http1_std_string_copy", "memory_lower_bound", iterations, baselineBytes.size(), copyReference);
    }

    void BenchmarkHttp2() {
        constexpr std::uint64_t iterations = 250000; // 小帧 build+parse 热路径轮次
        LikesProgram::Http::Http2Frame frame;
        frame.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Data);
        frame.flags = 0x1;
        frame.streamId = 1;
        frame.payload.resize(256, 0xA5);

        const auto baselineBytes = RequireValue(LikesProgram::Http::BuildHttp2Frame(frame), "HTTP/2 benchmark baseline build failed");

        const auto roundTrip = Measure([&] {
            std::uint64_t checksum = 0; // 累积帧长度和 stream id
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const auto built = LikesProgram::Http::BuildHttp2Frame(frame);
                if (!built.IsOk()) throw std::runtime_error("HTTP/2 benchmark build failed");
                const auto parsed = LikesProgram::Http::ParseHttp2Frame(built.Value());
                if (!parsed.IsOk()) throw std::runtime_error("HTTP/2 benchmark parse failed");
                checksum += parsed.Value().payload.size() + parsed.Value().streamId;
            }
            return checksum;
        });
        PrintMeasurement("http2_build_parse", "protocol_codec", iterations,
            baselineBytes.size(), roundTrip);

        const auto copyReference = Measure([&] {
            std::uint64_t checksum = 0; // std::vector 复制只作为内存下界
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const std::vector<std::uint8_t> copy(baselineBytes);
                checksum += copy.size() + copy.front();
            }
            return checksum;
        });
        PrintMeasurement("http2_std_vector_copy", "memory_lower_bound", iterations, baselineBytes.size(), copyReference);
    }

    void BenchmarkHttp3() {
        constexpr std::uint64_t iterations = 250000; // QUIC varint 帧 build+parse 热路径轮次
        LikesProgram::Http::Http3Frame frame;
        frame.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
        frame.payload.resize(256, 0x3C);

        const auto baselineBytes = RequireValue(LikesProgram::Http::BuildHttp3Frame(frame), "HTTP/3 benchmark baseline build failed");

        const auto roundTrip = Measure([&] {
            std::uint64_t checksum = 0; // 累积帧类型和正文长度
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const auto built = LikesProgram::Http::BuildHttp3Frame(frame);
                if (!built.IsOk()) throw std::runtime_error("HTTP/3 benchmark build failed");
                const auto parsed = LikesProgram::Http::ParseHttp3Frame(built.Value());
                if (!parsed.IsOk()) throw std::runtime_error("HTTP/3 benchmark parse failed");
                checksum += parsed.Value().payload.size() + parsed.Value().type;
            }
            return checksum;
        });
        PrintMeasurement("http3_build_parse", "protocol_codec", iterations,
            baselineBytes.size(), roundTrip);

        const auto copyReference = Measure([&] {
            std::uint64_t checksum = 0; // std::vector 复制只作为内存下界
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const std::vector<std::uint8_t> copy(baselineBytes);
                checksum += copy.size() + copy.front();
            }
            return checksum;
        });
        PrintMeasurement("http3_std_vector_copy", "memory_lower_bound", iterations, baselineBytes.size(), copyReference);
    }

    void BenchmarkSession() {
        constexpr std::uint64_t iterations = 1000000; // Session 虚调用与 Result 路径轮次
        BenchmarkTransport transport;
        LikesProgram::Http::HttpSession session(&transport);
        session.SetVersion(LikesProgram::Http::HttpVersion::Http3);

        LikesProgram::Http::HttpRequest request;
        request.method = "GET";
        request.target = "/session-benchmark";

        const auto sessionMeasurement = Measure([&] {
            std::uint64_t checksum = 0; // 累积响应码和正文长度
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const auto response = session.Send(request);
                if (!response.IsOk()) throw std::runtime_error("Session benchmark Send failed");
                checksum += static_cast<std::uint64_t>(response.Value().statusCode) + response.Value().body.size();
            }
            return checksum;
        });
        PrintMeasurement("session_send", "session_dispatch", iterations, request.target.size(), sessionMeasurement);

        const auto directMeasurement = Measure([&] {
            std::uint64_t checksum = 0; // 直接虚调用作为 Session 额外开销下界
            for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
                const auto response = transport.Exchange(request, LikesProgram::Http::HttpVersion::Http3);
                if (!response.IsOk()) throw std::runtime_error("Direct transport benchmark failed");
                checksum += static_cast<std::uint64_t>(response.Value().statusCode) + response.Value().body.size();
            }
            return checksum;
        });
        PrintMeasurement("session_direct_transport", "dispatch_lower_bound", iterations, request.target.size(), directMeasurement);
    }
}

int main() {
    try {
        std::cout << "benchmark_scope=http_codec_and_session\n";
#ifdef NDEBUG
        std::cout << "build_type=release\n";
#else
        std::cout << "build_type=non_release\n";
#endif
        std::cout << "comparison_note=memory_and_dispatch_references_are_lower_bounds_not_full_http_implementations\n";
        BenchmarkHttp1();
        BenchmarkHttp2();
        BenchmarkHttp3();
        BenchmarkSession();
    } catch (const std::exception& ex) {
        std::cerr << "LikesProgramHttpBenchmark failed: " << ex.what() << '\n';
        return 1;
    }

    return g_benchmarkSink == 0 ? 2 : 0;
}
