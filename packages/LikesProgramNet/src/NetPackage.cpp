#include <LikesProgram/Net/Net.hpp>
#include <LikesProgram/Core/Version.hpp>

namespace LikesProgram {
    namespace Net {
        const char* PackageName() noexcept {
            return "LikesProgramNet";
        }

        const char* PackageVersion() noexcept {
            return LikesProgram::Version::CurrentString().data();
        }

        bool PackageAvailable() noexcept {
            return true;
        }
    }
}
