#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Core/Version.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

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
            endpoint.Set(u"name", LikesProgram::Config::ConfigValue(
                LikesProgram::String::Format(u"node{}", i)));
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
        Require(std::strcmp(packageVersion, LikesProgram::Version::CurrentString().data()) == 0,
            "Config package version should follow Core version");
    }

    void TestSetGetAndRemove() {
        LikesProgram::Config::Configuration config;
        config.Set(u"service.name", u"orders");
        config.Set(u" service.port ", u"8080");
        config.Set(u"", u"ignored");

        Require(config.Size() == 2, "Config should ignore empty keys");
        Require(config.Contains(u"service.name"), "Config should contain service.name");
        Require(config.Contains(u"service.port"), "Config should trim keys");
        Require(config.GetString(u"service.name") == u"orders", "Config string value mismatch");
        Require(config.GetInt64(u"service.port") == 8080, "Config int64 value mismatch");

        config.Set(u"service.port", u"9090");
        Require(config.Size() == 2, "Config Set should overwrite existing key");
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
        Require(std::fabs(config.GetDouble(u"ratio", 0.0) - 0.75) < 0.000001,
            "Config double parsing mismatch");
        Require(config.GetBool(u"enabled", false), "Config bool ON should parse true");
        Require(!config.GetBool(u"disabled", true), "Config bool no should parse false");
        Require(config.GetInt64(u"invalid_int", 42) == 42, "Invalid int should return default");
        Require(config.GetBool(u"invalid_bool", true), "Invalid bool should return default");
        Require(config.GetString(u"missing", u"default") == u"default",
            "Missing string should return default");
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
        Require(config.GetString(u"empty", u"default") == u"default",
            "Empty parsed value should use default string");

        LikesProgram::String roundTrip = config.ToKeyValueLines();
        Require(roundTrip.Find(u"service.name=orders") != LikesProgram::String::npos,
            "Round trip should contain service.name");
        Require(roundTrip.Find(u"threads=12") != LikesProgram::String::npos,
            "Round trip should contain overwritten threads");
        Require(roundTrip.Find(u"unicode.emoji=😀") != LikesProgram::String::npos,
            "Round trip should contain Unicode value");
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
        Require(compact.Find(u"\"service\"") != LikesProgram::String::npos,
            "JSON serialization should keep object fields");
        Require(compact.Find(u"\"items\"") != LikesProgram::String::npos,
            "JSON serialization should keep arrays");

        auto bad = LikesProgram::Config::Configuration::TryFromJson(u"{\"a\": [1,}");
        Require(!bad.IsOk(), "JSON parser should reject malformed arrays");
        Require(bad.GetStatus().Message().Find(u"line") != LikesProgram::String::npos,
            "JSON parser should report line/column diagnostics");
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
        Require(config.Root().Get(u"items").At(1).AsString() == u"worker",
            "YAML array item mismatch");

        LikesProgram::String out = config.ToYaml();
        Require(out.Find(u"service:") != LikesProgram::String::npos,
            "YAML serialization should include object key");
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
        Require(config.Root().Get(u"feature.labels").At(0).AsString() == u"api",
            "TOML array mismatch");
        Require(config.GetInt64(u"feature.limits.cpu") == 2, "TOML inline table mismatch");

        LikesProgram::String out = config.ToToml();
        Require(out.Find(u"title = \"orders\"") != LikesProgram::String::npos,
            "TOML serialization should include root scalar");
        Require(out.Find(u"labels = [\"api\", \"worker\"]") != LikesProgram::String::npos,
            "TOML serialization should keep scalar arrays");

        auto reparsed = LikesProgram::Config::Configuration::FromToml(out);
        Require(reparsed.GetString(u"title") == u"orders", "TOML round trip root scalar mismatch");
        Require(reparsed.GetInt64(u"service.port") == 8080, "TOML round trip dotted key mismatch");
        Require(reparsed.Root().Get(u"feature.labels").At(1).AsString() == u"worker",
            "TOML round trip array mismatch");
        Require(reparsed.GetString(u"feature.limits.memory") == u"512Mi",
            "TOML round trip inline table mismatch");
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
        Require(config.GetInt64(u"service.limits.cpu.count") == 2,
            "TOML inline dotted key should create nested object");
        Require(config.Root().Get(u"service.limits").Get(u"memory.limit").AsString() == u"512Mi",
            "TOML quoted dotted inline key should remain literal");
        Require(config.Root().Get(u"service").Get(u"key=with.equals").AsString() == u"safe",
            "TOML quoted key containing equals should parse");
        Require(config.Root().Get(u"service.endpoints").At(0).Get(u"name").AsString() == u"node0",
            "TOML array table first item mismatch");
        Require(config.Root().Get(u"service.endpoints").At(1).Get(u"weight").AsInt64() == 2,
            "TOML array table second item mismatch");

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
        Require(jsonRoundTrip.Root().Get(u"service.endpoints").At(2).Get(u"name").AsString() == u"node2",
            "JSON round trip array object mismatch");

        LikesProgram::String yaml = config.ToYaml();
        auto yamlRoundTrip = LikesProgram::Config::Configuration::FromYaml(yaml);
        Require(yamlRoundTrip.GetInt64(u"service.port") == 8080, "YAML round trip int mismatch");
        Require(yamlRoundTrip.Root().Get(u"service.labels").At(0).AsString() == u"api",
            "YAML round trip array mismatch");

        LikesProgram::String toml = config.ToToml();
        Require(toml.Find(u"[service.metadata]") != LikesProgram::String::npos,
            "TOML serialization should include nested table");
        Require(toml.Find(u"\"owner.name\" = \"platform\"") != LikesProgram::String::npos,
            "TOML serialization should quote dotted key segment");
        Require(toml.Find(u"endpoints = [{") != LikesProgram::String::npos,
            "TOML serialization should keep arrays of inline tables");

        auto tomlRoundTrip = LikesProgram::Config::Configuration::FromToml(toml);
        Require(tomlRoundTrip.GetString(u"title") == u"orders", "TOML round trip root mismatch");
        Require(tomlRoundTrip.Root().Get(u"service.metadata").Get(u"owner.name").AsString() == u"platform",
            "TOML round trip quoted dotted key mismatch");
        Require(tomlRoundTrip.Root().Get(u"service.endpoints").At(1).Get(u"weight").AsInt64() == 2,
            "TOML round trip array object mismatch");
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
        auto config = LikesProgram::Config::Configuration::FromJson(
            u"{\"service\":{\"name\":\"orders\",\"port\":8080},\"feature\":{}}\n");

        auto serviceSchema = LikesProgram::Config::ConfigSchema::ObjectType()
            .Required(u"name", LikesProgram::Config::ConfigSchema::StringType())
            .Required(u"port", LikesProgram::Config::ConfigSchema::Int64Type())
            .AllowUnknownKeys(false);

        auto schema = LikesProgram::Config::ConfigSchema::ObjectType()
            .Required(u"service", serviceSchema)
            .Optional(u"feature", LikesProgram::Config::ConfigSchema::ObjectType())
            .Optional(u"workers", LikesProgram::Config::ConfigSchema::Int64Type(),
                LikesProgram::Config::ConfigValue(4))
            .AllowUnknownKeys(false);

        auto validation = config.Validate(schema);
        Require(validation.IsOk(), "Schema should accept valid config");

        config.ApplyDefaults(schema);
        Require(config.GetInt64(u"workers") == 4, "Schema should apply default values");

        auto invalid = LikesProgram::Config::Configuration::FromJson(
            u"{\"service\":{\"name\":7},\"extra\":true}");
        auto invalidResult = invalid.Validate(schema);
        Require(!invalidResult.IsOk(), "Schema should reject missing and mistyped fields");
        Require(invalidResult.Report().Find(u"service.name") != LikesProgram::String::npos,
            "Schema diagnostics should include nested path");
        Require(invalidResult.Report().Find(u"extra") != LikesProgram::String::npos,
            "Schema diagnostics should include unknown key");
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
        TestYamlSupport();
        TestTomlSupport();
        TestTomlIndustrialEdges();
        TestRoundTripSerialization();
        TestMalformedInputs();
        TestSchemaValidation();
    }
    catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
