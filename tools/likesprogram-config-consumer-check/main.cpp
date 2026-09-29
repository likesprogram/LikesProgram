#include <LikesProgram/Config/Config.hpp>
#include <iostream>

int main() {
    if (!LikesProgram::Config::PackageAvailable()) return 1;

    const auto keyValue = LikesProgram::Config::Configuration::FromKeyValueLines(
        u"service.name=config-consumer\n"
        u"service.port=7001\n"
        u"feature.enabled=true\n"); // 验证 key=value 外部消费方主路径

    if (keyValue.GetString(u"service.name") != u"config-consumer") return 2;
    if (keyValue.GetInt64(u"service.port") != 7001) return 3;
    if (!keyValue.GetBool(u"feature.enabled")) return 4;

    const auto json = LikesProgram::Config::Configuration::TryFromJson5(
        u"{release:{channel:'stable',build:+1,},}"); // 验证安装头和动态符号暴露 JSON5 能力
    if (!json.IsOk()) return 5;
    if (json.Value().GetString(u"release.channel") != u"stable") return 6;

    const auto jsonRoundTrip = LikesProgram::Config::Configuration::TryFromJson5(
        json.Value().ToJson5(-1));
    if (!jsonRoundTrip.IsOk() || jsonRoundTrip.Value().GetInt64(u"release.build") != 1) return 7;

    const auto yaml = LikesProgram::Config::Configuration::TryFromYaml(
        u"release:\n"
        u"  channel: stable\n"
        u"  build: 1\n"
        u"labels:\n"
        u"  - canary\n"
        u"  - blue\n");
    if (!yaml.IsOk() || yaml.Value().GetString(u"release.channel") != u"stable" ||
        yaml.Value().Root().Get(u"labels").Size() != 2) return 8;
    const auto yamlRoundTrip = LikesProgram::Config::Configuration::TryFromYaml(
        yaml.Value().ToYaml());
    if (!yamlRoundTrip.IsOk() || yamlRoundTrip.Value().GetString(u"release.channel") != u"stable") return 9;

    const auto toml = LikesProgram::Config::Configuration::TryFromToml(
        u"[release]\n"
        u"channel = \"stable\"\n"
        u"build = 1\n"
        u"labels = [\"canary\", \"blue\"]\n");
    if (!toml.IsOk() || toml.Value().GetString(u"release.channel") != u"stable" ||
        toml.Value().Root().Get(u"release.labels").Size() != 2) return 10;
    const auto tomlRoundTrip = LikesProgram::Config::Configuration::TryFromToml(
        toml.Value().ToToml());
    if (!tomlRoundTrip.IsOk() || tomlRoundTrip.Value().GetInt64(u"release.build") != 1) return 11;

    const auto schema = LikesProgram::Config::ConfigSchema::ObjectType()
        .Required(u"release", LikesProgram::Config::ConfigSchema::ObjectType()
            .Required(u"channel", LikesProgram::Config::ConfigSchema::StringType())
            .Required(u"build", LikesProgram::Config::ConfigSchema::Int64Type())
            .Optional(u"labels", LikesProgram::Config::ConfigSchema::ArrayType(
                LikesProgram::Config::ConfigSchema::StringType()))
            .AllowUnknownKeys(false))
        .AllowUnknownKeys(false);
    if (!toml.Value().Validate(schema).IsOk()) return 12;

    std::cout << LikesProgram::Config::PackageName()
        << " consumer check passed\n";
    return 0;
}
