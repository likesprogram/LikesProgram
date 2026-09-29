#include <LikesProgram/Http/Http.hpp>
#include <LikesProgram/Core/Version.hpp>

namespace LikesProgram {
    namespace Http {
        const char* PackageName() noexcept {
            // 包名固定为 C 字符串，避免跨动态库传递分配所有权。
            return "LikesProgramHttp";
        }

        const char* PackageVersion() noexcept {
            // Http 跟随 LikesProgram 统一版本，发布批次整体校验。
            return LikesProgram::Version::CurrentString().data();
        }

        bool PackageAvailable() noexcept {
            // 导出稳定符号，供测试和诊断工具确认 target 已链接。
            return true;
        }
    }
}
