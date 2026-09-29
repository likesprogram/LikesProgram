#include "net/platform/windows/WindowsPollerBackendPolicy.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << std::endl;
    std::exit(1);
}

} // namespace

int main() {
    // Windows 原生后端默认选择 IOCP，外来后端必须明确拒绝。
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest(nullptr)
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::Iocp,
        "Null backend should select IOCP");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("auto")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::Iocp,
        "Auto backend should select IOCP");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("iocp")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::Iocp,
        "Explicit IOCP should be supported");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("future-value")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::Iocp,
        "Unknown backend should preserve native default");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("epoll")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::UnsupportedKnown,
        "Known foreign backend must fail");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("io_uring")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::UnsupportedKnown,
        "Known foreign backend must fail");
    Require(
        LikesProgram::Net::Internal::ParseWindowsPollerBackendRequest("kqueue")
            == LikesProgram::Net::Internal::WindowsPollerBackendRequest::UnsupportedKnown,
        "Known foreign backend must fail");
    return 0;
}
