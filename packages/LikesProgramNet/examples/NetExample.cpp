#include <LikesProgram/Net/Net.hpp>
#include <iostream>

int main() {
    LikesProgram::Net::Buffer buffer;
    buffer.Append("net", 3);

    LikesProgram::Net::Address loopback("127.0.0.1", 0);

    std::cout << LikesProgram::Net::PackageName()
        << " " << LikesProgram::Net::PackageVersion()
        << " address=" << loopback.ToString()
        << " buffer=" << buffer.AsStringView()
        << '\n';

    return 0;
}
