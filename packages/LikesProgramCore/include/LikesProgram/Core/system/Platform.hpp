#pragma once
#include <string_view>

namespace LikesProgram {
    namespace Platform {
        // 操作系统枚举用于公共诊断和平台分支。
        enum class OperatingSystem {
            Windows,
            Linux,
            MacOS,
            Unix,
            Unknown
        };

        // CPU 架构枚举用于公共诊断和构建产物说明。
        enum class Architecture {
            X86,
            X64,
            Arm,
            Arm64,
            Wasm,
            Unknown
        };

        // 编译器枚举用于公共诊断和 ABI 约束说明。
        enum class Compiler {
            MSVC,
            Clang,
            GCC,
            Unknown
        };

#if defined(_WIN32)
        inline constexpr OperatingSystem CurrentOperatingSystem = OperatingSystem::Windows;
#elif defined(__APPLE__) && defined(__MACH__)
        inline constexpr OperatingSystem CurrentOperatingSystem = OperatingSystem::MacOS;
#elif defined(__linux__)
        inline constexpr OperatingSystem CurrentOperatingSystem = OperatingSystem::Linux;
#elif defined(__unix__)
        inline constexpr OperatingSystem CurrentOperatingSystem = OperatingSystem::Unix;
#else
        inline constexpr OperatingSystem CurrentOperatingSystem = OperatingSystem::Unknown;
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
        inline constexpr Architecture CurrentArchitecture = Architecture::X64;
#elif defined(_M_IX86) || defined(__i386__)
        inline constexpr Architecture CurrentArchitecture = Architecture::X86;
#elif defined(_M_ARM64) || defined(__aarch64__)
        inline constexpr Architecture CurrentArchitecture = Architecture::Arm64;
#elif defined(_M_ARM) || defined(__arm__)
        inline constexpr Architecture CurrentArchitecture = Architecture::Arm;
#elif defined(__wasm__)
        inline constexpr Architecture CurrentArchitecture = Architecture::Wasm;
#else
        inline constexpr Architecture CurrentArchitecture = Architecture::Unknown;
#endif

#if defined(_MSC_VER)
        inline constexpr Compiler CurrentCompiler = Compiler::MSVC;
#elif defined(__clang__)
        inline constexpr Compiler CurrentCompiler = Compiler::Clang;
#elif defined(__GNUC__)
        inline constexpr Compiler CurrentCompiler = Compiler::GCC;
#else
        inline constexpr Compiler CurrentCompiler = Compiler::Unknown;
#endif

#if defined(LIKESPROGRAM_CORE_SHARED)
        inline constexpr bool CoreShared = true;
#else
        inline constexpr bool CoreShared = false;
#endif

#if defined(_MSVC_LANG)
        inline constexpr long CxxStandard = _MSVC_LANG; // MSVC 下反映实际 /std 设置
#else
        inline constexpr long CxxStandard = __cplusplus; // 当前编译单元看到的 C++ 标准值
#endif

        // 返回当前操作系统名称。
        inline constexpr std::string_view OperatingSystemName() noexcept {
            switch (CurrentOperatingSystem) {
            case OperatingSystem::Windows: return "Windows";
            case OperatingSystem::Linux: return "Linux";
            case OperatingSystem::MacOS: return "MacOS";
            case OperatingSystem::Unix: return "Unix";
            case OperatingSystem::Unknown: return "Unknown";
            }
            return "Unknown";
        }

        // 返回当前 CPU 架构名称。
        inline constexpr std::string_view ArchitectureName() noexcept {
            switch (CurrentArchitecture) {
            case Architecture::X86: return "x86";
            case Architecture::X64: return "x64";
            case Architecture::Arm: return "arm";
            case Architecture::Arm64: return "arm64";
            case Architecture::Wasm: return "wasm";
            case Architecture::Unknown: return "unknown";
            }
            return "unknown";
        }

        // 返回当前编译器名称。
        inline constexpr std::string_view CompilerName() noexcept {
            switch (CurrentCompiler) {
            case Compiler::MSVC: return "MSVC";
            case Compiler::Clang: return "Clang";
            case Compiler::GCC: return "GCC";
            case Compiler::Unknown: return "Unknown";
            }
            return "Unknown";
        }

        // 判断当前是否为 Windows。
        inline constexpr bool IsWindows() noexcept {
            return CurrentOperatingSystem == OperatingSystem::Windows;
        }

        // 判断当前是否为类 Unix 平台。
        inline constexpr bool IsUnixLike() noexcept {
            switch (CurrentOperatingSystem) {
            case OperatingSystem::Linux:
            case OperatingSystem::MacOS:
            case OperatingSystem::Unix:
                return true;
            case OperatingSystem::Windows:
            case OperatingSystem::Unknown:
                return false;
            }
            return false;
        }
    }
}
