#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <vector>

namespace {
    // Config 包回归测试覆盖轻量 key=value、JSON/YAML/TOML 往返和 Schema 聚合错误。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // 构造跨 JSON/YAML/TOML 的嵌套样本，集中验证对象、数组和带特殊字符 key 的往返。
    LikesProgram::Config::Configuration BuildNestedConfig() {
        LikesProgram::Config::ConfigValue root = LikesProgram::Config::ConfigValue::Object();
        root.Set(u"title", LikesProgram::Config::ConfigValue(u"orders"));

        LikesProgram::Config::ConfigValue service = LikesProgram::Config::ConfigValue::Object();
        service.Set(u"name", LikesProgram::Config::ConfigValue(u"gateway"));
        service.Set(u"port", LikesProgram::Config::ConfigValue(8080));
        service.Set(u"enabled", LikesProgram::Config::ConfigValue(true));

        LikesProgram::Config::ConfigValue metadata = LikesProgram::Config::ConfigValue::Object();
        metadata.Set(u"owner.name", LikesProgram::Config::ConfigValue(u"platform"));
        metadata.Set(u"retry-count", LikesProgram::Config::ConfigValue(3));
        service.Set(u"metadata", metadata);

        LikesProgram::Config::ConfigValue labels = LikesProgram::Config::ConfigValue::Array();
        labels.PushBack(LikesProgram::Config::ConfigValue(u"api"));
        labels.PushBack(LikesProgram::Config::ConfigValue(u"worker"));
        service.Set(u"labels", labels);

        LikesProgram::Config::ConfigValue endpoints = LikesProgram::Config::ConfigValue::Array();
        for (int i = 0; i < 3; ++i) {
            LikesProgram::Config::ConfigValue endpoint = LikesProgram::Config::ConfigValue::Object();
            endpoint.Set(u"name", LikesProgram::Config::ConfigValue(LikesProgram::String::Format(u"node{}", i)));
            endpoint.Set(u"weight", LikesProgram::Config::ConfigValue(i + 1));
            endpoints.PushBack(endpoint);
        }
        service.Set(u"endpoints", endpoints);

        root.Set(u"service", service);
        return LikesProgram::Config::Configuration(root);
    }

    void TestPackageIdentity() {
        const char* packageName = LikesProgram::Config::PackageName();
        const char* packageVersion = LikesProgram::Config::PackageVersion();

        Require(LikesProgram::Config::PackageAvailable(), "Config package should be available");
        Require(std::strcmp(packageName, "LikesProgramConfig") == 0, "Config package name mismatch");
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0, "Config package version should follow Core version");
    }

    void TestSetGetAndRemove() {
        LikesProgram::Config::Configuration config;
        config.Set(u"service.name", u"orders");
        config.Set(u" service.port ", u"8080");
        config.Set(u"", u"ignored");

        Require(config.Size() == 1, "Config dotted keys should share a nested root object");
        Require(config.Contains(u"service.name"), "Config should contain service.name");
        Require(config.Contains(u"service.port"), "Config should trim keys");
        Require(config.GetString(u"service.name") == u"orders", "Config string value mismatch");
        Require(config.GetInt64(u"service.port") == 8080, "Config int64 value mismatch");
        Require(config.Root().Get(u"service").IsObject(), "Config dotted keys should create an object path");
        Require(config.Root().Get(u"service").Size() == 2, "Config nested object should retain both fields");

        config.Set(u"service.port", u"9090");
        Require(config.Size() == 1, "Config Set should overwrite an existing nested key");
        Require(config.GetInt64(u"service.port") == 9090, "Config overwrite mismatch");

        Require(config.Remove(u"service.port"), "Config Remove should report existing key");
        Require(!config.Contains(u"service.port"), "Config Remove should delete key");
        Require(!config.Remove(u"service.port"), "Config Remove should report missing key");

        config.Clear();
        Require(config.Size() == 0, "Config Clear should remove all keys");
    }

    void TestTypedDefaults() {
        LikesProgram::Config::Configuration config;
        config.Set(u"threads", u"16");
        config.Set(u"ratio", u"0.75");
        config.Set(u"enabled", u"ON");
        config.Set(u"disabled", u"no");
        config.Set(u"invalid_int", u"12x");
        config.Set(u"invalid_bool", u"maybe");

        Require(config.GetInt64(u"threads", -1) == 16, "Config int64 parsing mismatch");
        Require(std::fabs(config.GetDouble(u"ratio", 0.0) - 0.75) < 0.000001, "Config double parsing mismatch");
        Require(config.GetBool(u"enabled", false), "Config bool ON should parse true");
        Require(!config.GetBool(u"disabled", true), "Config bool no should parse false");
        Require(config.GetInt64(u"invalid_int", 42) == 42, "Invalid int should return default");
        Require(config.GetBool(u"invalid_bool", true), "Invalid bool should return default");
        Require(config.GetString(u"missing", u"default") == u"default", "Missing string should return default");
    }

    void TestKeyValueLines() {
        LikesProgram::String text =
            u"# comment\n"
            u"service.name = orders\n"
            u"threads=8\n"
            u"malformed\n"
            u"threads = 12\n"
            u"unicode.emoji=😀\n"
            u"empty = \n";

        auto config = LikesProgram::Config::Configuration::FromKeyValueLines(text);
        Require(config.Size() == 4, "Config parser should ignore comments and malformed lines");
        Require(config.GetString(u"service.name") == u"orders", "Parsed string value mismatch");
        Require(config.GetInt64(u"threads") == 12, "Duplicate key should keep latest value");
        Require(config.GetString(u"unicode.emoji") == u"😀", "Parser should preserve Unicode values");
        Require(config.GetString(u"empty", u"default") == u"default", "Empty parsed value should use default string");

        LikesProgram::String roundTrip = config.ToKeyValueLines();
        Require(roundTrip.Find(u"service.name=orders") != LikesProgram::String::npos, "Round trip should contain service.name");
        Require(roundTrip.Find(u"threads=12") != LikesProgram::String::npos, "Round trip should contain overwritten threads");
        Require(roundTrip.Find(u"unicode.emoji=😀") != LikesProgram::String::npos, "Round trip should contain Unicode value");
    }

    void TestCompatibilityAlias() {
        LikesProgram::Configuration config;
        config.Set(u"alias", u"ok");

        Require(config.GetString(u"alias") == u"ok", "LikesProgram::Configuration alias should work");
    }

    void TestJsonValueTree() {
        LikesProgram::String text =
            u"{"
                u"\"service\":{\"name\":\"orders\",\"port\":8080},"
                u"\"feature\":{\"enabled\":true},"
                u"\"items\":[\"a\",2,false],"
                u"\"unicode\":\"\\uD83D\\uDE00\""
            u"}";

        auto result = LikesProgram::Config::Configuration::TryFromJson(text);
        Require(result.IsOk(), "JSON parser should accept nested object");

        auto config = result.Value();
        Require(config.GetString(u"service.name") == u"orders", "JSON dotted string mismatch");
        Require(config.GetInt64(u"service.port") == 8080, "JSON dotted int mismatch");
        Require(config.GetBool(u"feature.enabled"), "JSON dotted bool mismatch");
        Require(config.Root().Get(u"items").At(1).AsInt64() == 2, "JSON array item mismatch");
        Require(config.GetString(u"unicode") == u"😀", "JSON unicode escape mismatch");

        LikesProgram::String compact = config.ToJson(-1);
        Require(compact.Find(u"\"service\"") != LikesProgram::String::npos, "JSON serialization should keep object fields");
        Require(compact.Find(u"\"items\"") != LikesProgram::String::npos, "JSON serialization should keep arrays");

        auto bad = LikesProgram::Config::Configuration::TryFromJson(u"{\"a\": [1,}");
        Require(!bad.IsOk(), "JSON parser should reject malformed arrays");
        Require(bad.GetStatus().Message().Find(u"line") != LikesProgram::String::npos, "JSON parser should report line/column diagnostics");

        // 根级字符串和空数组使用专用快路径，仍须保持完整 JSON 语义与诊断。
        const auto rootString = LikesProgram::Config::ConfigValue::TryParseJson(u"\"worker-0000\"");
        Require(rootString.IsOk() && rootString.Value().AsString() == u"worker-0000", "JSON parser should preserve an unescaped root string");
        const auto escapedRootString = LikesProgram::Config::ConfigValue::TryParseJson(u"\"line\\nvalue\"");
        Require(escapedRootString.IsOk() && escapedRootString.Value().AsString() == u"line\nvalue", "JSON parser should preserve an escaped root string");
        const auto emptyArray = LikesProgram::Config::ConfigValue::TryParseJson(u" [ \n ] ");
        Require(emptyArray.IsOk() && emptyArray.Value().IsArray() && emptyArray.Value().Size() == 0, "JSON parser should preserve a whitespace-padded empty root array");
        const auto trailingRootString = LikesProgram::Config::ConfigValue::TryParseJson(u"\"worker\" null");
        Require(!trailingRootString.IsOk() && trailingRootString.GetStatus().Message().Find(u"unexpected data after JSON value") != LikesProgram::String::npos, "JSON root string fast path should reject trailing input");

        const auto exactKeys = LikesProgram::Config::Configuration::FromJson(u"{\"\":1,\" spaced \":2,\"duplicate\":1,\"duplicate\":3,\"raw\":\"😀\"}");
        const auto exactRoot = exactKeys.Root();
        Require(exactRoot.Contains(u""), "JSON parser should preserve an empty object key");
        Require(exactRoot.Get(u"").AsInt64() == 1, "JSON empty object key value mismatch");
        Require(exactRoot.Contains(u" spaced "), "JSON parser should preserve key boundary spaces");
        Require(exactRoot.Get(u" spaced ").AsInt64() == 2, "JSON spaced object key value mismatch");
        Require(exactRoot.Get(u"duplicate").AsInt64() == 3, "JSON duplicate key should keep latest value");
        Require(exactRoot.Get(u"raw").AsString() == u"😀", "JSON raw surrogate pair should round trip");
        const auto exactText = exactKeys.ToJson(-1);
        Require(exactText.Find(u"\"\":1") != LikesProgram::String::npos, "JSON serializer should keep an empty object key");
        Require(exactText.Find(u"\" spaced \":2") != LikesProgram::String::npos, "JSON serializer should keep key boundary spaces");

        const auto numeric = LikesProgram::Config::Configuration::FromJson(u"{\"max\":9223372036854775807,\"min\":-9223372036854775808,\"float\":1.25e2}");
        Require(numeric.GetInt64(u"max") == std::numeric_limits<int64_t>::max(), "JSON parser should preserve INT64_MAX");
        Require(numeric.GetInt64(u"min") == std::numeric_limits<int64_t>::min(), "JSON parser should preserve INT64_MIN");
        Require(std::fabs(numeric.GetDouble(u"float") - 125.0) < 0.000001, "JSON parser should preserve exponent floats");
        const auto floatingEdges = LikesProgram::Config::ConfigValue::TryParseJson(u"[-0.5,6.022e23,1.7976931348623157e308,4.9406564584124654e-324,1e-4000]");
        Require(floatingEdges.IsOk(), "JSON parser should accept finite floating boundaries");
        Require(floatingEdges.Value().At(0).AsDouble() == -0.5, "JSON parser should preserve negative fractions");
        Require(floatingEdges.Value().At(1).AsDouble() == 6.022e23, "JSON parser should preserve positive exponents");
        Require(floatingEdges.Value().At(2).AsDouble() == std::numeric_limits<double>::max(), "JSON parser should preserve the largest finite double");
        Require(floatingEdges.Value().At(3).AsDouble() == std::numeric_limits<double>::denorm_min(), "JSON parser should preserve the smallest subnormal double");
        Require(floatingEdges.Value().At(4).AsDouble() == 0.0, "JSON parser should preserve finite underflow as zero");
        const auto floatingOverflow = LikesProgram::Config::ConfigValue::TryParseJson(u"1.7976931348623159e308");
        Require(!floatingOverflow.IsOk(), "JSON parser should reject floating overflow");
        Require(floatingOverflow.GetStatus().Message().Find(u"invalid floating number") != LikesProgram::String::npos, "JSON floating overflow should preserve its diagnostic category");
        Require(!LikesProgram::Config::Configuration::TryFromJson(u"{\"overflow\":9223372036854775808}").IsOk(), "JSON parser should reject positive int64 overflow");
        Require(!LikesProgram::Config::Configuration::TryFromJson(u"{\"overflow\":-9223372036854775809}").IsOk(), "JSON parser should reject negative int64 overflow");

        LikesProgram::Config::ConfigValue detached;
        {
            LikesProgram::String source = u"{\"long-key-value\":\"worker-1234567890-value\",\"escaped\":\"line\\nvalue\"}";
            auto parsed = LikesProgram::Config::ConfigValue::TryParseJson(source);
            Require(parsed.IsOk(), "JSON string ownership sample should parse");
            detached = parsed.MoveValue();
            source = LikesProgram::String(4096, u'x');
        }
        Require(detached.Get(u"long-key-value").AsString() == u"worker-1234567890-value", "Unescaped JSON strings should outlive their parse source");
        Require(detached.Get(u"escaped").AsString() == u"line\nvalue", "Escaped JSON strings should preserve decoded ownership");

        LikesProgram::String mediumSource =
            u"{\"mediumkey\":\"worker-0000\",\"duplicate9\":\"first-000\","
            u"\"duplicate9\":\"second-00\",\"ascii16\":\"abcdefghijklmnop\","
            u"\"ascii17\":\"abcdefghijklmnopq\",\"unicode9\":\"ééééééééé\","
            u"\"inline8\":\"12345678\",\"inline10\":\"1234567890\","
            u"\"inline11\":\"12345678901\",\"heap12\":\"123456789012\","
            u"\"surrogate11\":\"😀123456789\"}";
        auto mediumParsed = LikesProgram::Config::ConfigValue::TryParseJson(mediumSource);
        Require(mediumParsed.IsOk(), "Medium ASCII JSON sample should parse");
        LikesProgram::Config::ConfigValue mediumCopy = mediumParsed.Value();
        LikesProgram::Config::ConfigValue mediumMoved = mediumParsed.MoveValue();
        mediumSource = LikesProgram::String(4096, u'x');
        Require(mediumMoved.Get(u"mediumkey").AsString() == u"worker-0000", "Medium ASCII value should outlive its parse source");
        Require(mediumMoved.Get(u"duplicate9").AsString() == u"second-00", "Medium ASCII duplicate key should keep the latest value");
        Require(mediumMoved.Get(u"ascii16").AsString() == u"abcdefghijklmnop", "Sixteen-byte ASCII boundary should materialize exactly");
        Require(mediumMoved.Get(u"ascii17").AsString() == u"abcdefghijklmnopq", "Seventeen-byte ASCII fallback should materialize exactly");
        Require(mediumMoved.Get(u"unicode9").AsString() == u"ééééééééé", "Non-ASCII medium fallback should preserve Unicode");
        Require(mediumMoved.Get(u"inline8").AsString() == u"12345678", "Eight-unit inline boundary should materialize exactly");
        Require(mediumMoved.Get(u"inline10").AsString() == u"1234567890", "Ten-unit inline value should materialize exactly");
        Require(mediumMoved.Get(u"inline11").AsString() == u"12345678901", "Eleven-unit inline boundary should materialize exactly");
        Require(mediumMoved.Get(u"heap12").AsString() == u"123456789012", "Twelve-unit heap fallback should materialize exactly");
        Require(mediumMoved.Get(u"surrogate11").AsString() == u"😀123456789", "Eleven-unit surrogate value should preserve its UTF-16 pair");
        Require(mediumCopy == mediumMoved, "Medium ASCII copy and move should preserve value semantics");
        auto mediumRoundTrip = LikesProgram::Config::ConfigValue::TryParseJson(mediumMoved.ToJson(-1));
        Require(mediumRoundTrip.IsOk() && mediumRoundTrip.Value() == mediumMoved, "Medium ASCII JSON serialization should round trip");
    }

    // JSON5 主路径集中覆盖宽松语法、兼容入口、序列化和失败诊断。
    void TestJson5Support() {
        const LikesProgram::String text =
            u"\uFEFF/* config header */{\n"
            u"  // service options\n"
            u"  service: {name: 'orders', port: +0x1F, ratio: .75, whole: 1.,},\n"
            u"  list: ['api', 'worker',],\n"
            u"  escaped: 'A\\x42\\v',\n"
            u"  continued: 'line\\\nnext',\n"
            u"  \\u006bey: 'decoded',\n"
            u"  infinity: Infinity,\n"
            u"  negativeInfinity: -Infinity,\n"
            u"  nan: NaN,\n"
            u"}";

        auto result = LikesProgram::Config::Configuration::TryFromJson5(text);
        Require(result.IsOk(), "JSON5 parser should accept its common syntax extensions");
        const auto config = result.Value();
        Require(config.GetString(u"service.name") == u"orders", "JSON5 single-quoted string or identifier key mismatch");
        Require(config.GetInt64(u"service.port") == 31, "JSON5 signed hexadecimal integer mismatch");
        Require(std::fabs(config.GetDouble(u"service.ratio") - 0.75) < 0.000001 && config.GetDouble(u"service.whole") == 1.0, "JSON5 leading or trailing decimal point mismatch");
        Require(config.Root().Get(u"list").Size() == 2, "JSON5 array trailing comma mismatch");
        Require(config.GetString(u"escaped") == u"AB\v", "JSON5 hexadecimal or vertical-tab string escape mismatch");
        Require(config.GetString(u"continued") == u"linenext", "JSON5 string line continuation mismatch");
        Require(config.GetString(u"key") == u"decoded", "JSON5 escaped identifier key mismatch");
        Require(std::isinf(config.GetDouble(u"infinity")) && config.GetDouble(u"infinity") > 0 && std::isinf(config.GetDouble(u"negativeInfinity")) && config.GetDouble(u"negativeInfinity") < 0 && std::isnan(config.GetDouble(u"nan")), "JSON5 non-finite number mismatch");

        const auto compatible = LikesProgram::Config::Configuration::TryFromJson(u"{answer: +42, trailing: true,}");
        Require(compatible.IsOk() && compatible.Value().GetInt64(u"answer") == 42, "Legacy Json entry points should accept JSON5 without a source migration");
        const auto throwingAlias = LikesProgram::Config::Configuration::FromJson5(u"{value:'configuration-alias'}");
        const auto valueAlias = LikesProgram::Config::ConfigValue::FromJson5(u"['value-alias',]");
        Require(throwingAlias.GetString(u"value") == u"configuration-alias" && valueAlias.At(0).AsString() == u"value-alias" && valueAlias.ToJson5(-1).Find(u"value-alias") != LikesProgram::String::npos, "JSON5 throwing aliases should expose the same parser behavior");
        const auto rootString = LikesProgram::Config::ConfigValue::TryParseJson5(u"'root\\x21'");
        Require(rootString.IsOk() && rootString.Value().AsString() == u"root!", "JSON5 root string fast path should accept single quotes and hexadecimal escapes");

        const auto lineSeparator = LikesProgram::Config::ConfigValue::TryParseJson5(u"{first:1,// comment\u2028\u3000second:2}");
        Require(lineSeparator.IsOk() && lineSeparator.Value().Get(u"second").AsInt64() == 2, "JSON5 comments and Unicode whitespace should preserve the next field");

        const auto serialized = config.ToJson5(-1);
        Require(serialized.Find(u"Infinity") != LikesProgram::String::npos && serialized.Find(u"-Infinity") != LikesProgram::String::npos && serialized.Find(u"NaN") != LikesProgram::String::npos, "JSON5 serialization should preserve non-finite numbers");
        const auto roundTrip = LikesProgram::Config::ConfigValue::TryParseJson5(serialized);
        Require(roundTrip.IsOk() && std::isnan(roundTrip.Value().Get(u"nan").AsDouble()), "JSON5 explicit aliases should round trip serialized values");

        const auto unterminatedComment = LikesProgram::Config::ConfigValue::TryParseJson5(u"{answer: 42} /* missing terminator");
        Require(!unterminatedComment.IsOk() && unterminatedComment.GetStatus().Message().Find(u"unterminated comment") != LikesProgram::String::npos, "JSON5 parser should reject an unterminated trailing comment");
        const char16_t* commentFailures[] = {
            u"/* missing terminator",
            u"'root' /* missing terminator",
            u"[] /* missing terminator",
            u"{value: /* missing terminator",
            u"{value: 1, /* missing terminator",
            u"[1 /* missing terminator",
            u"[1, /* missing terminator"
        };
        for (const auto* failure : commentFailures) {
            const auto failed = LikesProgram::Config::ConfigValue::TryParseJson5(failure);
            Require(!failed.IsOk() && failed.GetStatus().Message().Find(u"unterminated comment") != LikesProgram::String::npos, "JSON5 parser should preserve nested and root comment failures");
        }
    }

    // 普通 JSON 语法失败通过显式状态逐层返回，诊断类别和后续解析能力必须保持稳定。
    void TestJsonErrorStates() {
        struct FailureCase {
            const char16_t* text; // 待拒绝的 JSON 文档
            const char16_t* diagnostic; // 预期诊断片段
        };

        const FailureCase cases[] = {
            { u"", u"unexpected end of input" },
            { u"?", u"invalid JSON value" },
            { u"tru", u"invalid literal" },
            { u"{\"a\":1,,}", u"expected object key string or identifier" },
            { u"{\"a\" 1}", u"expected ':' after object key" },
            { u"{\"a\":1", u"unexpected end in object" },
            { u"{\"a\":1 \"b\":2}", u"expected ',' between object fields" },
            { u"[1,", u"unexpected end of input" },
            { u"[1 2]", u"expected ',' between array values" },
            { u"\"line\nbreak\"", u"control character in string" },
            { u"\"\\u12\"", u"incomplete unicode escape" },
            { u"\"\\u12G4\"", u"invalid unicode escape digit" },
            { u"\"\\uD800\"", u"expected low surrogate after high surrogate" },
            { u"\"\\uD800\\u0041\"", u"invalid low surrogate" },
            { u"\"\\uDC00\"", u"low surrogate without high surrogate" },
            { u"\"\\x\"", u"incomplete hexadecimal escape" },
            { u"\"unterminated", u"unterminated string" },
            { u"-", u"incomplete number" },
            { u"01", u"leading zero is not allowed" },
            { u".", u"expected digit after decimal point" },
            { u"1e+", u"expected exponent digit" },
            { u"9223372036854775808", u"integer number is out of int64 range" },
            { u"true false", u"unexpected data after JSON value" }
        };

        for (const auto& failure : cases) {
            auto result = LikesProgram::Config::ConfigValue::TryParseJson(failure.text);
            Require(!result.IsOk(), "Invalid JSON case should return a failed Result");
            Require(result.GetStatus().Message().Find(failure.diagnostic) != LikesProgram::String::npos, "Invalid JSON case should preserve its diagnostic category");
        }

        const auto multiline = LikesProgram::Config::ConfigValue::TryParseJson(u"{\n  \"items\": [1,\n  }");
        Require(!multiline.IsOk(), "Multiline invalid JSON should fail");
        Require(multiline.GetStatus().Message() == u"JSON parse error at line 3, column 3: invalid JSON value", "JSON diagnostics should preserve exact line, column, and message text");

        LikesProgram::String wideColumn(1024, u' ');
        wideColumn.Append(u'?');
        const auto largeColumn = LikesProgram::Config::ConfigValue::TryParseJson(wideColumn);
        Require(!largeColumn.IsOk(), "Long-prefix invalid JSON should fail");
        Require(largeColumn.GetStatus().Message() == u"JSON parse error at line 1, column 1025: invalid JSON value", "JSON diagnostics should format multi-digit columns exactly");

        // 延迟诊断必须自包含行列信息，不能借用已经销毁的临时输入。
        std::u16string temporaryFailure(2048, u' '); // 离开调用表达式后立即释放的长输入
        temporaryFailure[0] = u'\n';
        temporaryFailure.back() = u'?';
        auto delayedStatus = LikesProgram::Config::ConfigValue::TryParseJson(LikesProgram::String(temporaryFailure)); // 仅保留失败 Result，输入临时对象随即销毁
        std::vector<LikesProgram::String> releasedStorageReuse; // 覆盖已释放输入缓冲的同尺寸分配
        releasedStorageReuse.reserve(32);
        for (int i = 0; i < 32; ++i) releasedStorageReuse.emplace_back(temporaryFailure.size(), u'x');
        Require(!delayedStatus.IsOk(), "Temporary JSON input should return a failed Result");
        Require(delayedStatus.GetStatus().Message() == u"JSON parse error at line 2, column 2047: invalid JSON value", "Deferred JSON diagnostics should outlive temporary input storage");

        std::u16string deepFailure;
        for (int depth = 0; depth < 64; ++depth) deepFailure.append(u"{\"level\":");
        deepFailure.append(u"[1,2,}");
        for (int depth = 0; depth < 64; ++depth) deepFailure.push_back(u'}');
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto result = LikesProgram::Config::ConfigValue::TryParseJson(LikesProgram::String(deepFailure));
            Require(!result.IsOk(), "Repeated deep JSON failure should remain stable");
            Require(result.GetStatus().Message().Find(u"invalid JSON value") != LikesProgram::String::npos, "Deep JSON failure should preserve the innermost diagnostic");
        }

        auto recovered = LikesProgram::Config::ConfigValue::TryParseJson(u"{\"service\":{\"name\":\"orders\"},\"items\":[1,2,3]}");
        Require(recovered.IsOk(), "Valid JSON should parse after repeated explicit failures");
        Require(recovered.Value().Get(u"service.name").AsString() == u"orders", "Recovered JSON tree should preserve nested values");
    }

    void TestYamlSupport() {
        LikesProgram::String text =
            u"service:\n"
            u"  name: orders\n"
            u"  port: 8080\n"
            u"feature:\n"
            u"  enabled: true\n"
            u"items:\n"
            u"  - api\n"
            u"  - worker\n";

        auto config = LikesProgram::Config::Configuration::FromYaml(text);
        Require(config.GetString(u"service.name") == u"orders", "YAML nested string mismatch");
        Require(config.GetInt64(u"service.port") == 8080, "YAML nested int mismatch");
        Require(config.GetBool(u"feature.enabled"), "YAML nested bool mismatch");
        Require(config.Root().Get(u"items").At(1).AsString() == u"worker", "YAML array item mismatch");

        LikesProgram::String out = config.ToYaml();
        Require(out.Find(u"service:") != LikesProgram::String::npos, "YAML serialization should include object key");
    }

    void TestTomlSupport() {
        LikesProgram::String text =
            u"title = \"orders\"\n"
            u"service.port = 8080\n"
            u"[feature]\n"
            u"enabled = true\n"
            u"labels = [\"api\", \"worker\"]\n"
            u"limits = { cpu = 2, memory = \"512Mi\" }\n";

        auto config = LikesProgram::Config::Configuration::FromToml(text);
        Require(config.GetString(u"title") == u"orders", "TOML root string mismatch");
        Require(config.GetInt64(u"service.port") == 8080, "TOML dotted key mismatch");
        Require(config.GetBool(u"feature.enabled"), "TOML table bool mismatch");
        Require(config.Root().Get(u"feature.labels").At(0).AsString() == u"api", "TOML array mismatch");
        Require(config.GetInt64(u"feature.limits.cpu") == 2, "TOML inline table mismatch");

        LikesProgram::String out = config.ToToml();
        Require(out.Find(u"title = \"orders\"") != LikesProgram::String::npos, "TOML serialization should include root scalar");
        Require(out.Find(u"labels = [\"api\", \"worker\"]") != LikesProgram::String::npos, "TOML serialization should keep scalar arrays");

        auto reparsed = LikesProgram::Config::Configuration::FromToml(out);
        Require(reparsed.GetString(u"title") == u"orders", "TOML round trip root scalar mismatch");
        Require(reparsed.GetInt64(u"service.port") == 8080, "TOML round trip dotted key mismatch");
        Require(reparsed.Root().Get(u"feature.labels").At(1).AsString() == u"worker", "TOML round trip array mismatch");
        Require(reparsed.GetString(u"feature.limits.memory") == u"512Mi", "TOML round trip inline table mismatch");
    }

    void TestTomlIndustrialEdges() {
        LikesProgram::String text =
            u"title = \"orders\"\n"
            u"[service]\n"
            u"name = \"gateway\"\n"
            u"limits = { cpu.count = 2, \"memory.limit\" = \"512Mi\" }\n"
            u"\"key=with.equals\" = \"safe\"\n"
            u"[[service.endpoints]]\n"
            u"name = \"node0\"\n"
            u"weight = 1\n"
            u"[[service.endpoints]]\n"
            u"name = \"node1\"\n"
            u"weight = 2\n";

        auto config = LikesProgram::Config::Configuration::FromToml(text);
        Require(config.GetString(u"title") == u"orders", "TOML industrial root mismatch");
        Require(config.GetString(u"service.name") == u"gateway", "TOML industrial table mismatch");
        Require(config.GetInt64(u"service.limits.cpu.count") == 2, "TOML inline dotted key should create nested object");
        Require(config.Root().Get(u"service.limits").Get(u"memory.limit").AsString() == u"512Mi", "TOML quoted dotted inline key should remain literal");
        Require(config.Root().Get(u"service").Get(u"key=with.equals").AsString() == u"safe", "TOML quoted key containing equals should parse");
        Require(config.Root().Get(u"service.endpoints").At(0).Get(u"name").AsString() == u"node0", "TOML array table first item mismatch");
        Require(config.Root().Get(u"service.endpoints").At(1).Get(u"weight").AsInt64() == 2, "TOML array table second item mismatch");

        auto duplicate = LikesProgram::Config::Configuration::TryFromToml(u"a = 1\na = 2\n");
        Require(!duplicate.IsOk(), "TOML parser should reject duplicate keys");

        auto tableConflict = LikesProgram::Config::Configuration::TryFromToml(u"a = 1\n[a]\nb = 2\n");
        Require(!tableConflict.IsOk(), "TOML parser should reject scalar/table conflicts");

        auto bareString = LikesProgram::Config::Configuration::TryFromToml(u"name = orders\n");
        Require(!bareString.IsOk(), "TOML parser should reject bare strings");

        auto invalidUnicode = LikesProgram::Config::Configuration::TryFromToml(u"name = \"\\uD800\"\n");
        Require(!invalidUnicode.IsOk(), "TOML parser should reject invalid unicode scalar values");
    }

    void TestRoundTripSerialization() {
        auto config = BuildNestedConfig();

        LikesProgram::String json = config.ToJson(-1);
        auto jsonRoundTrip = LikesProgram::Config::Configuration::FromJson(json);
        Require(jsonRoundTrip.GetString(u"title") == u"orders", "JSON round trip root mismatch");
        Require(jsonRoundTrip.GetString(u"service.name") == u"gateway", "JSON round trip object mismatch");
        Require(jsonRoundTrip.Root().Get(u"service.endpoints").At(2).Get(u"name").AsString() == u"node2", "JSON round trip array object mismatch");

        LikesProgram::String yaml = config.ToYaml();
        auto yamlRoundTrip = LikesProgram::Config::Configuration::FromYaml(yaml);
        Require(yamlRoundTrip.GetInt64(u"service.port") == 8080, "YAML round trip int mismatch");
        Require(yamlRoundTrip.Root().Get(u"service.labels").At(0).AsString() == u"api", "YAML round trip array mismatch");

        LikesProgram::String toml = config.ToToml();
        Require(toml.Find(u"[service.metadata]") != LikesProgram::String::npos, "TOML serialization should include nested table");
        Require(toml.Find(u"\"owner.name\" = \"platform\"") != LikesProgram::String::npos, "TOML serialization should quote dotted key segment");
        Require(toml.Find(u"endpoints = [{") != LikesProgram::String::npos, "TOML serialization should keep arrays of inline tables");

        auto tomlRoundTrip = LikesProgram::Config::Configuration::FromToml(toml);
        Require(tomlRoundTrip.GetString(u"title") == u"orders", "TOML round trip root mismatch");
        Require(tomlRoundTrip.Root().Get(u"service.metadata").Get(u"owner.name").AsString() == u"platform", "TOML round trip quoted dotted key mismatch");
        Require(tomlRoundTrip.Root().Get(u"service.endpoints").At(1).Get(u"weight").AsInt64() == 2, "TOML round trip array object mismatch");
    }

    void TestMalformedInputs() {
        auto badYaml = LikesProgram::Config::Configuration::TryFromYaml(u"root:\n\tbad: true\n");
        Require(!badYaml.IsOk(), "YAML parser should reject tab indentation");

        auto badToml = LikesProgram::Config::Configuration::TryFromToml(u"[service]\n[[service]]\nname = \"x\"\n");
        Require(!badToml.IsOk(), "TOML parser should reject table/array table conflicts");

        auto badTomlKey = LikesProgram::Config::Configuration::TryFromToml(u"\"bad.key = 1\n");
        Require(!badTomlKey.IsOk(), "TOML parser should reject unterminated quoted key");

        auto badJson = LikesProgram::Config::Configuration::TryFromJson(u"{\"x\":\"\\uD800\"}");
        Require(!badJson.IsOk(), "JSON parser should reject lone high surrogate");
    }

    void TestSchemaValidation() {
        auto config = LikesProgram::Config::Configuration::FromJson(u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"feature\":{}}\n");

        auto serviceSchema = LikesProgram::Config::ConfigSchema::ObjectType()
            .Required(u"name", LikesProgram::Config::ConfigSchema::StringType())
            .Required(u"port", LikesProgram::Config::ConfigSchema::Int64Type())
            .AllowUnknownKeys(false);

        auto schema = LikesProgram::Config::ConfigSchema::ObjectType()
            .Required(u"service", serviceSchema)
            .Optional(u"feature", LikesProgram::Config::ConfigSchema::ObjectType())
            .Optional(u"workers", LikesProgram::Config::ConfigSchema::Int64Type(), LikesProgram::Config::ConfigValue(4))
            .AllowUnknownKeys(false);

        auto validation = config.Validate(schema);
        Require(validation.IsOk(), "Schema should accept valid config");

        config.ApplyDefaults(schema);
        Require(config.GetInt64(u"workers") == 4, "Schema should apply default values");

        auto invalid = LikesProgram::Config::Configuration::FromJson(u"{\"service\":{\"name\":7},\"extra\":true}");
        auto invalidResult = invalid.Validate(schema);
        Require(!invalidResult.IsOk(), "Schema should reject missing and mistyped fields");
        Require(invalidResult.Report().Find(u"service.name") != LikesProgram::String::npos, "Schema diagnostics should include nested path");
        Require(invalidResult.Report().Find(u"extra") != LikesProgram::String::npos, "Schema diagnostics should include unknown key");
    }

    void TestLargeDocumentsAndConcurrentReads() {
        std::u16string json = u"{\"service\":{\"name\":\"orders\"},\"items\":[";
        constexpr int itemCount = 2048;
        for (int i = 0; i < itemCount; ++i) {
            if (i != 0) json.push_back(u',');
            json.append(LikesProgram::String::Format(u"{{\"id\":{},\"enabled\":{},\"name\":\"worker{}\"}}", i, (i % 2) == 0, i).ToU16String());
        }
        json.append(u"]}");

        const LikesProgram::String largeJson(json);
        auto parsed = LikesProgram::Config::Configuration::TryFromJson(largeJson);
        Require(parsed.IsOk(), "Large JSON document should parse");
        const auto config = parsed.MoveValue();
        Require(config.Root().Get(u"items").Size() == itemCount, "Large JSON array size mismatch");
        Require(config.Root().Get(u"items").At(itemCount - 1).Get(u"id").AsInt64() == itemCount - 1, "Large JSON tail item mismatch");

        for (int i = 0; i < 4; ++i) {
            auto repeated = LikesProgram::Config::Configuration::TryFromJson(largeJson);
            Require(repeated.IsOk(), "Repeated large JSON parse should remain stable");
            Require(repeated.Value().Root().Get(u"items").Size() == itemCount, "Repeated large JSON size mismatch");
        }

        const LikesProgram::String yaml =
            u"service:\n"
            u"  name: orders\n"
            u"items:\n"
            u"  - one\n"
            u"  - two\n";
        auto yamlResult = LikesProgram::Config::Configuration::TryFromYaml(yaml);
        Require(yamlResult.IsOk(), "YAML document should parse after large JSON loads");
        Require(yamlResult.Value().Root().Get(u"items").Size() == 2, "YAML array size mismatch after repeated loads");

        const LikesProgram::String toml =
            u"title = \"orders\"\n"
            u"[service]\n"
            u"name = \"gateway\"\n"
            u"items = [{ id = 1, enabled = true }, { id = 2, enabled = false }]\n";
        auto tomlResult = LikesProgram::Config::Configuration::TryFromToml(toml);
        Require(tomlResult.IsOk(), "TOML document should parse after large JSON loads");
        Require(tomlResult.Value().Root().Get(u"service.items").Size() == 2, "TOML inline array size mismatch after repeated loads");

        const auto concurrentConfig = LikesProgram::Config::Configuration::FromJson(u"{\"service\":{\"name\":\"orders\"},\"items\":[1,2]}");
        std::atomic<bool> failed{ false };
        std::vector<std::thread> readers;
        for (int worker = 0; worker < 8; ++worker) {
            readers.emplace_back([&concurrentConfig, &failed] {
                for (int i = 0; i < 2000; ++i) {
                    if (concurrentConfig.GetString(u"service.name") != u"orders" || !concurrentConfig.Contains(u"items") || concurrentConfig.Size() != 2) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& reader : readers) reader.join();
        Require(!failed.load(std::memory_order_relaxed), "Concurrent configuration reads should remain consistent");
    }

    // 递归内部节点必须继续保持公开深复制、脱离和 moved-from 恢复语义。
    void TestRecursiveNodeValueSemantics() {
        LikesProgram::Config::ConfigValue root = LikesProgram::Config::ConfigValue::Object(); // 原始对象树
        LikesProgram::Config::ConfigValue child = LikesProgram::Config::ConfigValue::Object(); // 待移动子树
        LikesProgram::Config::ConfigValue items = LikesProgram::Config::ConfigValue::Array(); // 待移动数组
        items.PushBack(LikesProgram::Config::ConfigValue(1));
        items.PushBack(LikesProgram::Config::ConfigValue(2));
        child.Set(u"name", LikesProgram::Config::ConfigValue(u"orders"));
        child.Set(u"items", std::move(items));
        Require(items.IsNull(), "PushBack/Set rvalue source should recover to null");
        root.Set(u"service", std::move(child));
        Require(child.IsNull(), "Set rvalue object source should recover to null");

        child = LikesProgram::Config::ConfigValue(u"reused");
        items = LikesProgram::Config::ConfigValue::Array();
        items.PushBack(LikesProgram::Config::ConfigValue(7));
        Require(child.AsString() == u"reused" && items.At(0).AsInt64() == 7, "Moved-from values should remain assignable and reusable");

        LikesProgram::Config::ConfigValue copy = root; // 显式 CloneNode 整树副本
        LikesProgram::Config::ConfigValue detached = root.Get(u"service"); // 脱离父树的子树副本
        LikesProgram::Config::ConfigValue detachedItem = detached.Get(u"items").At(1); // 脱离数组元素
        auto changed = copy.Get(u"service"); // 修改副本中的子树后再写回
        changed.Set(u"name", LikesProgram::Config::ConfigValue(u"billing"));
        copy.Set(u"service", std::move(changed));
        Require(root.Get(u"service").Get(u"name").AsString() == u"orders", "Changing a copied tree should not mutate the source tree");
        Require(detached.Get(u"name").AsString() == u"orders" && detachedItem.AsInt64() == 2, "Get and At results should remain independent from their parent tree");

        LikesProgram::Config::ConfigValue deep = LikesProgram::Config::ConfigValue(42); // 深层叶子
        for (int depth = 0; depth < 128; ++depth) {
            LikesProgram::Config::ConfigValue parent = LikesProgram::Config::ConfigValue::Object(); // 当前父层
            parent.Set(u"next", std::move(deep));
            deep = std::move(parent);
        }
        LikesProgram::Config::ConfigValue deepCopy = deep; // 深层显式递归克隆
        Require(deepCopy == deep && deepCopy.ToJson(-1) == deep.ToJson(-1), "Deep recursive node copy should preserve complete tree contents");
    }

    // 配置树可跨线程移动和销毁，短生命周期线程退出后仍须完整释放内部资源。
    void TestCrossThreadValueOwnership() {
        const LikesProgram::String fixture = u"{\"service\":{\"name\":\"orders\"},\"items\":[1,2,3,4]}";
        LikesProgram::Config::ConfigValue transferred; // 由生产线程写入、主线程接管的完整值树
        bool producerParsed = false; // join 后由主线程检查的生产结果

        std::thread producer([&] {
            auto parsed = LikesProgram::Config::ConfigValue::TryParseJson(fixture);
            if (!parsed.IsOk()) return;
            transferred = parsed.MoveValue();
            producerParsed = true;
        });
        producer.join();

        Require(producerParsed && transferred.Get(u"items").Size() == 4, "ConfigValue should survive transfer from its allocating thread");

        std::atomic<bool> consumerValidated{ false }; // 消费线程销毁跨线程值树前的语义检查
        std::thread consumer([value = std::move(transferred), &consumerValidated]() mutable {
            auto roundTrip = LikesProgram::Config::ConfigValue::TryParseJson(value.ToJson(-1));
            consumerValidated.store(roundTrip.IsOk() && roundTrip.Value() == value, std::memory_order_relaxed);
        });
        consumer.join();
        Require(consumerValidated.load(std::memory_order_relaxed), "Cross-thread ConfigValue destruction should preserve value semantics");

        std::atomic<int> completedWorkers{ 0 }; // 每个短生命周期线程完成全部重复解析后累加
        std::vector<std::thread> owners;
        for (int worker = 0; worker < 8; ++worker) {
            owners.emplace_back([fixture, &completedWorkers] {
                for (int iteration = 0; iteration < 32; ++iteration) {
                    auto parsed = LikesProgram::Config::ConfigValue::TryParseJson(fixture);
                    if (!parsed.IsOk() || parsed.Value().Get(u"items").Size() != 4) return;
                }
                completedWorkers.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& owner : owners) owner.join();
        Require(completedWorkers.load(std::memory_order_relaxed) == 8, "Short-lived ConfigValue owner threads should all complete cleanly");
    }
}

int main() {
    try {
        TestPackageIdentity();
        TestSetGetAndRemove();
        TestTypedDefaults();
        TestKeyValueLines();
        TestCompatibilityAlias();
        TestJsonValueTree();
        TestJson5Support();
        TestJsonErrorStates();
        TestYamlSupport();
        TestTomlSupport();
        TestTomlIndustrialEdges();
        TestRoundTripSerialization();
        TestMalformedInputs();
        TestSchemaValidation();
        TestLargeDocumentsAndConcurrentReads();
        TestRecursiveNodeValueSemantics();
        TestCrossThreadValueOwnership();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
