#include <LikesProgram/Config/Config.hpp>

#include <iostream>

int main() {
    LikesProgram::String text =
        u"# service settings\n"
        u"service.name=orders\n"
        u"service.port=8080\n"
        u"feature.enabled=true\n";

    auto config = LikesProgram::Config::Configuration::FromKeyValueLines(text);

    std::cout << LikesProgram::Config::PackageName() << " "
        << LikesProgram::Config::PackageVersion() << std::endl;
    std::cout << LikesProgram::String::Format(u"{}:{} enabled={}",
        config.GetString(u"service.name", u"unknown"),
        config.GetInt64(u"service.port", 80),
        config.GetBool(u"feature.enabled", false)).ToStdString() << std::endl;

    return 0;
}
