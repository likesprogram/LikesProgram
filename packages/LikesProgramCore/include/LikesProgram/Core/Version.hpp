#pragma once
#include <string_view>

namespace LikesProgram {
    namespace Version {
        // 版本信息值对象，用于 CMake 包版本和运行时展示保持一致。
        struct Info {
            int major = 0;                         // 主版本号，破坏性变更时递增
            int minor = 0;                         // 次版本号，功能兼容扩展时递增
            int patch = 0;                         // 修订版本号，修复和小改动时递增
            std::string_view suffix;               // 预发布或构建后缀，稳定版为空
        };

        inline constexpr int Major = 1;            // 当前统一主版本号
        inline constexpr int Minor = 0;            // 当前统一次版本号
        inline constexpr int Patch = 0;            // 当前统一修订版本号
        inline constexpr std::string_view Name = "LikesProgram";       // 生态名称
        inline constexpr std::string_view Suffix = "";                 // 稳定版无后缀
        inline constexpr std::string_view VersionString = "1.0.0";     // 语义化版本文本

        // 返回当前版本三元组。
        inline constexpr Info Current() noexcept {
            return Info{ Major, Minor, Patch, Suffix };
        }

        // 判断当前版本是否至少达到指定版本。
        inline constexpr bool IsAtLeast(int major, int minor, int patch) noexcept {
            if (Major != major) return Major > major;
            if (Minor != minor) return Minor > minor;
            return Patch >= patch;
        }

        // 返回可展示的版本字符串。
        inline constexpr std::string_view CurrentString() noexcept {
            return VersionString;
        }
    }
}
