#include <LikesProgram/Config/Config.hpp>

#include <nlohmann/json.hpp>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

// 该目标只在显式性能验收时构建，参考解析器不会进入 Config 产品依赖或安装导出。
namespace {
    volatile std::uint64_t g_sink = 0; // 保留每轮结果，阻止编译器消除完整解析或序列化。

    struct BenchmarkOptions {
        int iterations = 20; // 每个正式指标的重复次数
        std::string profileJsonScope; // 非空时只运行指定 LikesProgram JSON parse
    };

    std::string BuildJsonDocument(int count) {
        std::ostringstream text;
        text << "{\"service\":{\"name\":\"orders\",\"port\":8080},\"items\":[";
        for (int i = 0; i < count; ++i) {
            if (i > 0) text << ',';
            text << "{\"name\":\"worker" << i << "\",\"threads\":" << ((i % 8) + 1) << ",\"enabled\":" << ((i % 2) == 0 ? "true" : "false") << '}';
        }
        text << "]}";
        return text.str();
    }

    std::string BuildDeepJsonDocument(int depth) {
        std::string text;
        text.reserve(static_cast<size_t>(depth) * 12 + 16);
        for (int i = 0; i < depth; ++i) text += "{\"level\":";
        text += "{\"value\":42}";
        for (int i = 0; i < depth; ++i) text += '}';
        return text;
    }

    std::string BuildYamlDocument(int count) {
        std::ostringstream text;
        text << "service:\n  name: orders\n  port: 8080\nitems:\n";
        for (int i = 0; i < count; ++i) {
            text << "  - name: worker" << i << "\n"
                 << "    threads: " << ((i % 8) + 1) << "\n"
                 << "    enabled: " << ((i % 2) == 0 ? "true" : "false") << "\n";
        }
        return text.str();
    }

    BenchmarkOptions ParseOptions(int argc, char** argv) {
        BenchmarkOptions options; // 保留无参数时的完整成熟实现矩阵
        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);
            if (arg == "--iterations" && i + 1 < argc) options.iterations = std::atoi(argv[++i]);
            else if (arg == "--profile-json-scope" && i + 1 < argc) options.profileJsonScope = argv[++i];
            else throw std::invalid_argument("usage: ConfigReferenceBenchmark [--iterations N] " "[--profile-json-scope standard|deep|large]");
        }
        if (options.iterations <= 0) throw std::invalid_argument("iterations must be positive");
        if (!options.profileJsonScope.empty() && options.profileJsonScope != "standard" && options.profileJsonScope != "deep" && options.profileJsonScope != "large") {
            throw std::invalid_argument("profile JSON scope must be standard, deep, or large");
        }
        return options;
    }

    void Measure(
        const char* metric,
        const char* implementation,
        const char* format,
        const char* operation,
        int iterations,
        const std::function<std::uint64_t()>& fn) {
        for (int i = 0; i < 2; ++i) g_sink = g_sink + fn();
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) g_sink = g_sink + fn();
        const auto end = std::chrono::steady_clock::now();
        const auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
        const double perOperation = static_cast<double>(total) / static_cast<double>(iterations);
        std::cout << "metric=" << metric
                  << ",implementation=" << implementation
                  << ",format=" << format
                  << ",operation=" << operation
                  << ",iterations=" << iterations
                  << ",total_ns=" << total
                  << ",ns_per_op=" << perOperation
                  << '\n';
    }

    void BenchmarkJsonDocument(const char* scope, const std::string& json, int iterations) {
        const LikesProgram::String likesText(json);
        const auto likesConfig = LikesProgram::Config::Configuration::FromJson(likesText);
        const auto nlohmannValue = nlohmann::json::parse(json);
        rapidjson::Document rapidValue;
        rapidValue.Parse(json.data(), json.size());
        if (rapidValue.HasParseError()) throw std::runtime_error("RapidJSON fixture parse failed");

        const std::string parseMetric = std::string("json_") + scope + "_parse";
        Measure(parseMetric.c_str(), "likesprogram", "json", "parse", iterations, [&] {
            auto value = LikesProgram::Config::Configuration::FromJson(likesText);
            return static_cast<std::uint64_t>(value.Size());
        });
        Measure(parseMetric.c_str(), "nlohmann_json", "json", "parse", iterations, [&] {
            auto value = nlohmann::json::parse(json);
            return static_cast<std::uint64_t>(value.size());
        });
        Measure(parseMetric.c_str(), "rapidjson", "json", "parse", iterations, [&] {
            rapidjson::Document value;
            value.Parse(json.data(), json.size());
            return value.HasParseError() ? 0U : static_cast<std::uint64_t>(value.MemberCount());
        });

        if (std::string_view(scope) != "standard") return;
        Measure("json_standard_serialize", "likesprogram", "json", "serialize", iterations, [&] {
            return static_cast<std::uint64_t>(likesConfig.ToJson(-1).Length());
        });
        Measure("json_standard_serialize", "nlohmann_json", "json", "serialize", iterations, [&] {
            return static_cast<std::uint64_t>(nlohmannValue.dump().size());
        });
        Measure("json_standard_serialize", "rapidjson", "json", "serialize", iterations, [&] {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            rapidValue.Accept(writer);
            return static_cast<std::uint64_t>(buffer.GetSize());
        });
    }

    // Profiler-only entrypoint keeps reference parsers and serialization out of the sample.
    void ProfileLikesJsonDocument(const char* scope, const std::string& json, int iterations) {
        const LikesProgram::String likesText(json);
        const std::string metric = std::string("json_") + scope + "_parse";
        Measure(metric.c_str(), "likesprogram", "json", "parse", iterations, [&] {
            auto value = LikesProgram::Config::Configuration::FromJson(likesText);
            return static_cast<std::uint64_t>(value.Size());
        });
    }

    void BenchmarkInvalidJson(int iterations) {
        const std::string json = "{\"items\":[1,2,}";
        const LikesProgram::String likesText(json);
        Measure("json_invalid_parse", "likesprogram", "json", "invalid_parse", iterations, [&] {
            auto result = LikesProgram::Config::Configuration::TryFromJson(likesText);
            return result.IsOk() ? 0U : static_cast<std::uint64_t>(result.GetStatus().Message().Length() + 1U);
        });
        Measure("json_invalid_parse", "nlohmann_json", "json", "invalid_parse", iterations, [&] {
            try {
                auto value = nlohmann::json::parse(json);
                return static_cast<std::uint64_t>(value.size());
            }
            catch (const nlohmann::json::parse_error& error) {
                return static_cast<std::uint64_t>(error.byte + std::strlen(error.what()));
            }
        });
        Measure("json_invalid_parse", "rapidjson", "json", "invalid_parse", iterations, [&] {
            rapidjson::Document value;
            value.Parse(json.data(), json.size());
            if (!value.HasParseError()) return static_cast<std::uint64_t>(value.MemberCount());

            const size_t offset = value.GetErrorOffset(); // RapidJSON 的原始错误字节位置
            size_t line = 1; // 与 Config Status 相同的 1-based 行号
            size_t column = 1; // 与 Config Status 相同的 1-based 列号
            for (size_t i = 0; i < std::min(offset, json.size()); ++i) {
                if (json[i] == '\n') {
                    ++line;
                    column = 1;
                } else ++column;
            }
            const char* detail = rapidjson::GetParseError_En(value.GetParseError()); // 成熟库诊断文本
            return static_cast<std::uint64_t>(offset + line + column + std::strlen(detail));
        });
    }

    void BenchmarkYaml(const std::string& yaml, int iterations) {
        const LikesProgram::String likesText(yaml);
        const auto likesConfig = LikesProgram::Config::Configuration::FromYaml(likesText);
        const auto yamlValue = YAML::Load(yaml);
        Measure("yaml_standard_parse", "likesprogram", "yaml", "parse", iterations, [&] {
            auto value = LikesProgram::Config::Configuration::FromYaml(likesText);
            return static_cast<std::uint64_t>(value.Size());
        });
        Measure("yaml_standard_parse", "yaml_cpp", "yaml", "parse", iterations, [&] {
            auto value = YAML::Load(yaml);
            return static_cast<std::uint64_t>(value.size());
        });
        Measure("yaml_standard_serialize", "likesprogram", "yaml", "serialize", iterations, [&] {
            return static_cast<std::uint64_t>(likesConfig.ToYaml().Length());
        });
        Measure("yaml_standard_serialize", "yaml_cpp", "yaml", "serialize", iterations, [&] {
            YAML::Emitter emitter;
            emitter << yamlValue;
            return static_cast<std::uint64_t>(emitter.size());
        });
    }
}

int main(int argc, char** argv) {
    try {
        const BenchmarkOptions options = ParseOptions(argc, argv);
        std::cout << "benchmark_version=1"
                  << ",scope=same_document_preconverted_native_input"
                  << ",nlohmann_json=" << NLOHMANN_JSON_VERSION_MAJOR << '.'
                  << NLOHMANN_JSON_VERSION_MINOR << '.' << NLOHMANN_JSON_VERSION_PATCH
                  << ",rapidjson=" << RAPIDJSON_MAJOR_VERSION << '.'
                  << RAPIDJSON_MINOR_VERSION << '.' << RAPIDJSON_PATCH_VERSION
                  << ",yaml_cpp=" << LIKESPROGRAM_YAML_CPP_VERSION
                  << '\n';
        if (!options.profileJsonScope.empty()) {
            const std::string& scope = options.profileJsonScope;
            std::cout << "profile_mode=likesprogram_json,profile_scope=" << scope << '\n';
            if (scope == "standard") ProfileLikesJsonDocument("standard", BuildJsonDocument(256), options.iterations);
            else if (scope == "deep") ProfileLikesJsonDocument("deep", BuildDeepJsonDocument(48), options.iterations);
            else ProfileLikesJsonDocument("large", BuildJsonDocument(2048), options.iterations);
        }
        else {
            BenchmarkJsonDocument("standard", BuildJsonDocument(256), options.iterations);
            BenchmarkJsonDocument("deep", BuildDeepJsonDocument(48), options.iterations);
            BenchmarkJsonDocument("large", BuildJsonDocument(2048), options.iterations);
            BenchmarkInvalidJson(options.iterations);
            BenchmarkYaml(BuildYamlDocument(256), options.iterations);
        }
        std::cout << "sink=" << g_sink << '\n';
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Config reference benchmark failed: " << ex.what() << '\n';
        return 2;
    }
}
