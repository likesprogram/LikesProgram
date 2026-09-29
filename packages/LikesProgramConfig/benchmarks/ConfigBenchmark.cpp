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
        std::cout << name << " likes_ns=" << likesNs << " std_ns=" << stdNs << std::endl;
    }

    LikesProgram::String BuildJsonDocument(int count) {
        LikesProgram::String text = u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"items\":["; // JSON 文档缓冲
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(u',');
            text.Append(LikesProgram::String::Format(u"{{\"name\":\"worker{}\",\"threads\":{},\"enabled\":{}}}", i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false"));
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
                i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false")
            );
        }
        return text;
    }

    LikesProgram::String BuildJsonScalarArray(int count, bool strings, bool longStrings) {
        LikesProgram::String text = u"["; // 成功解析分解文档，不计入被测区间
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(u',');
            if (!strings) text.Append(LikesProgram::String::Format(u"{}", i));
            else if (longStrings) text.Append(LikesProgram::String::Format(u"\"worker-{:04}-value\"", i));
            else text.Append(LikesProgram::String::Format(u"\"w{:04}\"", i));
        }
        text.Append(u']');
        return text;
    }

    LikesProgram::String BuildJsonLiteralArray(int count, const char16_t* literal) {
        LikesProgram::String text = u"["; // 相同节点数下比较不同标量解析分支
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(u',');
            text.Append(std::u16string_view(literal));
        }
        text.Append(u']');
        return text;
    }

    LikesProgram::String BuildJsonFlatIntObject(int count, bool duplicateKeys) {
        LikesProgram::String text = u"{"; // 宽对象放大重复 key 查找与字段保留成本
        for (int i = 0; i < count; ++i) {
            if (i > 0) text.Append(u',');
            if (duplicateKeys) text.Append(LikesProgram::String::Format(u"\"same\":{}", i));
            else text.Append(LikesProgram::String::Format(u"\"k{:04}\":{}", i, i));
        }
        text.Append(u'}');
        return text;
    }

    LikesProgram::String BuildJsonDeepIntObject(int depth) {
        LikesProgram::String text;
        for (int i = 0; i < depth; ++i) text.Append(u"{\"level\":");
        text.Append(u'0');
        for (int i = 0; i < depth; ++i) text.Append(u'}');
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
                i, (i % 8) + 1, (i % 2) == 0 ? u"true" : u"false")
            );
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
            item.Set(u"name", LikesProgram::Config::ConfigValue(LikesProgram::String::Format(u"worker{}", i)));
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

    // 批量成功解析用于判断候选是否覆盖标准与大文档，减少单次计时噪声。
    void BenchmarkJsonSuccessBatch(const char* name, int count, int iterations) {
        const LikesProgram::String json = BuildJsonDocument(count);
        const auto likesParse = MeasureNs([&] {
            std::uint64_t itemCount = 0; // 累计成功结果，使每轮解析均保持可观察
            for (int i = 0; i < iterations; ++i) {
                auto result = LikesProgram::Config::ConfigValue::TryParseJson(json);
                if (result.IsOk()) itemCount += static_cast<std::uint64_t>(result.Value().Get(u"items").Size());
            }
            return itemCount + Probe();
        });
        Print(name, likesParse, 0);
    }

    void BenchmarkJsonParseCase(const char* name, const LikesProgram::String& json, int iterations) {
        const auto likesParse = MeasureNs([&] {
            std::uint64_t observed = 0; // 每轮结果状态、类型与大小均保持可观察
            for (int i = 0; i < iterations; ++i) {
                auto result = LikesProgram::Config::ConfigValue::TryParseJson(json);
                if (result.IsOk()) {
                    observed += static_cast<std::uint64_t>(result.Value().Type()) + 1U;
                    observed += static_cast<std::uint64_t>(result.Value().Size());
                }
            }
            return observed + Probe();
        });
        Print(name, likesParse, 0);
    }

    // 通过跨越 u16string SSO 与 ConfigText inline 阈值的文档分解成功解析成本。
    void BenchmarkJsonSuccessCases() {
        const LikesProgram::String emptyArray = u"[]";
        const LikesProgram::String shortString = u"\"worker\"";
        const LikesProgram::String longString = u"\"worker-1234567890-value\"";
        const LikesProgram::String nullArray = BuildJsonLiteralArray(256, u"null");
        const LikesProgram::String boolArray = BuildJsonLiteralArray(256, u"true");
        const LikesProgram::String intArray = BuildJsonScalarArray(256, false, false);
        const LikesProgram::String doubleArray = BuildJsonLiteralArray(256, u"1.25");
        const LikesProgram::String shortStringArray = BuildJsonScalarArray(256, true, false);
        const LikesProgram::String mediumStringArray = BuildJsonLiteralArray(256, u"\"worker-0000\"");
        const LikesProgram::String longStringArray = BuildJsonScalarArray(256, true, true);
        const LikesProgram::String uniqueIntObject = BuildJsonFlatIntObject(256, false);
        const LikesProgram::String duplicateIntObject = BuildJsonFlatIntObject(256, true);
        const LikesProgram::String deepIntObject = BuildJsonDeepIntObject(48);

        BenchmarkJsonParseCase("json_parse_empty_array_4096", emptyArray, 4096);
        BenchmarkJsonParseCase("json_parse_short_string_4096", shortString, 4096);
        BenchmarkJsonParseCase("json_parse_long_string_4096", longString, 4096);
        BenchmarkJsonParseCase("json_parse_null_array_64", nullArray, 64);
        BenchmarkJsonParseCase("json_parse_bool_array_64", boolArray, 64);
        BenchmarkJsonParseCase("json_parse_int_array_64", intArray, 64);
        BenchmarkJsonParseCase("json_parse_double_array_64", doubleArray, 64);
        BenchmarkJsonParseCase("json_parse_short_string_array_64", shortStringArray, 64);
        BenchmarkJsonParseCase("json_parse_medium_string_array_64", mediumStringArray, 64);
        BenchmarkJsonParseCase("json_parse_long_string_array_64", longStringArray, 64);
        BenchmarkJsonParseCase("json_parse_unique_int_object_64", uniqueIntObject, 64);
        BenchmarkJsonParseCase("json_parse_duplicate_int_object_64", duplicateIntObject, 64);
        BenchmarkJsonParseCase("json_parse_deep_int_object_256", deepIntObject, 256);
    }

    long long MeasureInvalidJson(const LikesProgram::String& invalidJson, int iterations) {
        return MeasureNs([&] {
            std::uint64_t failures = 0; // 保留失败诊断的可观察累积值
            for (int i = 0; i < iterations; ++i) {
                auto result = LikesProgram::Config::ConfigValue::TryParseJson(invalidJson);
                if (!result.IsOk()) failures += static_cast<std::uint64_t>(result.GetStatus().Message().Length() + 1U);
            }
            return failures + Probe();
        });
    }

    // 分解固定诊断、行列扫描和失败前部分树构造成本；这些口径只用于实验定位。
    void BenchmarkInvalidJson() {
        const LikesProgram::String rootInvalid = u"?"; // 未创建 ConfigValue 即失败
        LikesProgram::String prefixedInvalid; // 同样未创建节点，但迫使错误出口扫描长前缀
        for (int i = 0; i < 1024; ++i) prefixedInvalid.Append(u' ');
        prefixedInvalid.Append(u'?');

        const LikesProgram::String partialInvalid = u"{\"items\":[1,2,}"; // 正式 reference 错误文档
        LikesProgram::String nodesInvalid = u"{\"items\":["; // 构造 64 个节点后在数组尾失败
        for (int i = 0; i < 64; ++i) {
            if (i > 0) nodesInvalid.Append(u',');
            nodesInvalid.Append(LikesProgram::String::Format(u"{}", i));
        }
        nodesInvalid.Append(u",}");

        Print("json_invalid_root_4096", MeasureInvalidJson(rootInvalid, 4096), 0);
        Print("json_invalid_prefix1024_4096", MeasureInvalidJson(prefixedInvalid, 4096), 0);
        Print("json_invalid_partial_4096", MeasureInvalidJson(partialInvalid, 4096), 0);
        Print("json_invalid_nodes64_1024", MeasureInvalidJson(nodesInvalid, 1024), 0);

        // 保留产品基准和正式 reference 已使用的稳定名称与批次数。
        Print("json_invalid_parse_512", MeasureInvalidJson(partialInvalid, 512), 0);
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
    BenchmarkJsonSuccessBatch("json_parse_success_64", count, 64);
    BenchmarkJsonSuccessBatch("json_parse_large_success_16", 2048, 16);
    BenchmarkJsonSuccessCases();
    BenchmarkInvalidJson();
    BenchmarkYaml(count);
    BenchmarkToml(count);
    return 0;
}
