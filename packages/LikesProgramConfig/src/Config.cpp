#include <LikesProgram/Config/Config.hpp>
#include <LikesProgram/Core/Version.hpp>

namespace LikesProgram {
    namespace Config {
        const char* PackageName() noexcept {
            // 包名固定为 C 字符串，避免在动态库边界传递所有权。
            return "LikesProgramConfig";
        }

        const char* PackageVersion() noexcept {
            // Config 版本跟随 Core 的统一生态版本，发布时保持同批次验证。
            return LikesProgram::Version::CurrentString().data();
        }

        bool PackageAvailable() noexcept {
            // 导出一个稳定符号，供骨架阶段验证 target 与导出宏生效。
            return true;
        }
    }
}
