#include <LikesProgram/Config/Config.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#define LP_BENCH_NOINLINE __declspec(noinline)
#else
#define LP_BENCH_NOINLINE __attribute__((noinline))
#endif

// Config 基准用于观察解析、构建和序列化相对 std 容器/字符串构建的趋势。
namespace {
    volatile std::uint64_t g_probe = 0; // 防止编译器把微基准结果整体消除

    std::uint64_t Probe() {
        return g_probe;
    }

    template<typename F>
    long long MeasureNs(F&& fn) {
        auto begin = std::chrono::steady_clock::now(); // 微基准起点
        volatile std::uint64_t sink = fn();            // 保存可观察结果，避免优化掉被测逻辑
        (void)sink;
        auto end = std::chrono::steady_clock::now();   // 微基准终点
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }

    void Print(const char* name, long long likesNs, long long stdNs) {
        std::cout << name
            << " likes_ns=" << likesNs
            << " std_ns=" << stdNs
            << std::endl;
    }

    LikesProgram::String BuildJsonDocument(int count) {
        LikesProgram::String text = u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"items\":["; // JSON 文档缓冲
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(u',');
            text.Append(LikesProgram::String::Format(
                u"{{\"name\":\"worker{}\",\"threads\":{},\"enabled\":{}}}",
                i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false"));
        }
        text.Append(LikesProgram::String(u"]}"));
        return text;
    }

    LikesProgram::String BuildYamlDocument(int count) {
        LikesProgram::String text =
            u"service:\n"
            u"  name: orders\n"
            u"  port: 8080\n"
            u"items:\n"; // YAML 文档缓冲
        for (int i = 0; i < count; ++i) {
            text.Append(LikesProgram::String::Format(
                u"  - name: worker{}\n"
                u"    threads: {}\n"
                u"    enabled: {}\n",
                i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false"));
        }
        return text;
    }

    LikesProgram::String BuildTomlDocument(int count) {
        LikesProgram::String text =
            u"title = \"orders\"\n"
            u"[service]\n"
            u"name = \"orders\"\n"
            u"port = 8080\n"
            u"items = ["; // TOML 文档缓冲，数组元素使用 inline table 表达
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(LikesProgram::String(u", "));
            text.Append(LikesProgram::String::Format(
                u"{{ name = \"worker{}\", threads = {}, enabled = {} }}",
                i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false"));
        }
        text.Append(LikesProgram::String(u"]\n"));
        return text;
    }

    LikesProgram::Config::Configuration BuildConfigTree(int count) {
        LikesProgram::Config::ConfigValue root = LikesProgram::Config::ConfigValue::Object();
        LikesProgram::Config::ConfigValue service = LikesProgram::Config::ConfigValue::Object();
        service.Set(u"name", LikesProgram::Config::ConfigValue(u"orders"));
        service.Set(u"port", LikesProgram::Config::ConfigValue(8080));

        LikesProgram::Config::ConfigValue items = LikesProgram::Config::ConfigValue::Array();
        for (int i = 0; i < count; ++i) {
            LikesProgram::Config::ConfigValue item = LikesProgram::Config::ConfigValue::Object();
            item.Set(u"name", LikesProgram::Config::ConfigValue(
                LikesProgram::String::Format(u"worker{}", i)));
            item.Set(u"threads", LikesProgram::Config::ConfigValue((i % 8) + 1));
            item.Set(u"enabled", LikesProgram::Config::ConfigValue((i % 2) == 0));
            items.PushBack(std::move(item));
        }

        root.Set(u"service", std::move(service));
        root.Set(u"items", std::move(items));
        return LikesProgram::Config::Configuration(std::move(root));
    }

    LP_BENCH_NOINLINE std::vector<std::pair<std::u16string, std::u16string>> BuildStdPairs(int count) {
        std::vector<std::pair<std::u16string, std::u16string>> pairs; // std 容器构建基线
        pairs.reserve(static_cast<size_t>(count) * 3 + 2);
        pairs.emplace_back(u"service.name", u"orders");
        pairs.emplace_back(u"service.port", u"8080");
        for (int i = 0; i < count; ++i) {
            pairs.emplace_back(u"items.name", u"worker");
            pairs.emplace_back(u"items.threads", u"4");
            pairs.emplace_back(u"items.enabled", (i % 2) == 0 ? u"true" : u"false");
        }
        return pairs;
    }

    LP_BENCH_NOINLINE std::u16string BuildStdJsonLike(const std::vector<std::pair<std::u16string, std::u16string>>& pairs) {
        std::u16string output; // std 字符串拼接基线，不承担完整 JSON escaping 语义
        output.reserve(pairs.size() * 32);
        output += u"{";
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (i > 0) output += u",";
            output += u"\"";
            output += pairs[i].first;
            output += u"\":\"";
            output += pairs[i].second;
            output += u"\"";
        }
        output += u"}";
        return output;
    }

    void BenchmarkBuild(int count) {
        auto likes = MeasureNs([&] {
            auto config = BuildConfigTree(count);
            return static_cast<std::uint64_t>(config.Root().Get(u"items").Size()) + Probe();
        });

        auto std = MeasureNs([&] {
            auto pairs = BuildStdPairs(count);
            return static_cast<std::uint64_t>(pairs.size()) + Probe();
        });

        Print("construct_tree_vs_std_pairs", likes, std);
    }

    void BenchmarkJson(int count) {
        LikesProgram::String json = BuildJsonDocument(count);
        auto likesParse = MeasureNs([&] {
            auto config = LikesProgram::Config::Configuration::FromJson(json);
            return static_cast<std::uint64_t>(config.Root().Get(u"items").Size()) + Probe();
        });

        auto likesSerialize = MeasureNs([&] {
            auto config = BuildConfigTree(count);
            auto out = config.ToJson(-1);
            return static_cast<std::uint64_t>(out.Length()) + Probe();
        });

        auto std = MeasureNs([&] {
            auto pairs = BuildStdPairs(count);
            auto out = BuildStdJsonLike(pairs);
            return static_cast<std::uint64_t>(out.size()) + Probe();
        });

        Print("json_parse", likesParse, std);
        Print("json_serialize", likesSerialize, std);
    }

    void BenchmarkYaml(int count) {
        LikesProgram::String yaml = BuildYamlDocument(count);
        auto likesParse = MeasureNs([&] {
            auto config = LikesProgram::Config::Configuration::FromYaml(yaml);
            return static_cast<std::uint64_t>(config.Root().Get(u"items").Size()) + Probe();
        });

        auto likesSerialize = MeasureNs([&] {
            auto config = BuildConfigTree(count);
            auto out = config.ToYaml();
            return static_cast<std::uint64_t>(out.Length()) + Probe();
        });

        auto std = MeasureNs([&] {
            auto pairs = BuildStdPairs(count);
            return static_cast<std::uint64_t>(pairs.size()) + Probe();
        });

        Print("yaml_parse", likesParse, std);
        Print("yaml_serialize", likesSerialize, std);
    }

    void BenchmarkToml(int count) {
        LikesProgram::String toml = BuildTomlDocument(count);
        auto likesParse = MeasureNs([&] {
            auto config = LikesProgram::Config::Configuration::FromToml(toml);
            return static_cast<std::uint64_t>(config.Root().Get(u"service.items").Size()) + Probe();
        });

        auto likesSerialize = MeasureNs([&] {
            auto config = BuildConfigTree(count);
            auto out = config.ToToml();
            return static_cast<std::uint64_t>(out.Length()) + Probe();
        });

        auto std = MeasureNs([&] {
            auto pairs = BuildStdPairs(count);
            auto out = BuildStdJsonLike(pairs);
            return static_cast<std::uint64_t>(out.size()) + Probe();
        });

        Print("toml_parse", likesParse, std);
        Print("toml_serialize", likesSerialize, std);
    }
}

int main() {
    constexpr int count = 256; // 单轮基准的配置条目规模，足够暴露对象遍历放大问题
    BenchmarkBuild(count);
    BenchmarkJson(count);
    BenchmarkYaml(count);
    BenchmarkToml(count);
    return 0;
}
