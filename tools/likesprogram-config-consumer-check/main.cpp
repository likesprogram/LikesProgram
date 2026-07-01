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

    const auto json = LikesProgram::Config::Configuration::TryFromJson(
        u"{\"release\":{\"channel\":\"stable\",\"build\":1}}"); // 验证聚合头暴露 JSON 能力
    if (!json.IsOk()) return 5;
    if (json.Value().GetString(u"release.channel") != u"stable") return 6;

    std::cout << LikesProgram::Config::PackageName()
        << " consumer check passed\n";
    return 0;
}
