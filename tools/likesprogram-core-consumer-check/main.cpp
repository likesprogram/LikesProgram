#include <LikesProgram/Core/String.hpp>
#include <LikesProgram/Core/Version.hpp>
#include <iostream>
#include <string>

int main() {
    const std::string expected = std::string(LikesProgram::Version::Name)
        + " "
        + std::string(LikesProgram::Version::CurrentString()); // 期望的版本展示文本

    const LikesProgram::String name{ std::string(LikesProgram::Version::Name) }; // Core 名称参数
    const LikesProgram::String version{ std::string(LikesProgram::Version::CurrentString()) }; // 版本参数
    const std::string formatted = LikesProgram::String::Format(
        u"{} {}", name, version).ToStdString(); // 验证格式化和 String 导出
    if (formatted != expected) return 1;

    const std::string escaped = LikesProgram::String::EscapeJson(
        u"core \"ok\"").ToStdString(); // 验证公开 JSON 转义辅助能力
    if (escaped != "core \\\"ok\\\"") return 2;

    std::cout << expected << " core consumer check passed\n";
    return 0;
}
