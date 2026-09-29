#include "net/platform/linux/LinuxPollerBackendPolicy.hpp"
#include <cstdlib>

namespace {
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

int main() {
    using LikesProgram::Net::Internal::LinuxPollerBackend;
    using LikesProgram::Net::Internal::LinuxPollerBackendRequest;
    using LikesProgram::Net::Internal::ParseLinuxPollerBackendRequest;
    using LikesProgram::Net::Internal::SelectLinuxPollerBackend;

    Require(ParseLinuxPollerBackendRequest(nullptr) == LinuxPollerBackendRequest::Auto);
    Require(ParseLinuxPollerBackendRequest("") == LinuxPollerBackendRequest::Auto);
    Require(ParseLinuxPollerBackendRequest("auto") == LinuxPollerBackendRequest::Auto);
    Require(ParseLinuxPollerBackendRequest("io_uring") == LinuxPollerBackendRequest::IoUring);
    Require(ParseLinuxPollerBackendRequest("epoll") == LinuxPollerBackendRequest::Epoll);
    Require(ParseLinuxPollerBackendRequest("EPOLL") == LinuxPollerBackendRequest::Auto);
    Require(ParseLinuxPollerBackendRequest("invalid") == LinuxPollerBackendRequest::Auto);

    Require(SelectLinuxPollerBackend(LinuxPollerBackendRequest::Auto, true)
        == LinuxPollerBackend::IoUring);
    Require(SelectLinuxPollerBackend(LinuxPollerBackendRequest::Auto, false)
        == LinuxPollerBackend::Epoll);
    Require(SelectLinuxPollerBackend(LinuxPollerBackendRequest::IoUring, false)
        == LinuxPollerBackend::IoUring);
    Require(SelectLinuxPollerBackend(LinuxPollerBackendRequest::Epoll, true)
        == LinuxPollerBackend::Epoll);
    return 0;
}
