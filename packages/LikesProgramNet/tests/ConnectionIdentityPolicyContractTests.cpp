#include "net/ConnectionIdentityPolicy.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
    using LikesProgram::Net::Internal::MatchesWeakIdentity;
    using LikesProgram::Net::Internal::IsConnectionLeaseable;

    // 统一把连接身份索引失败转换为测试进程非零退出。
    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // fd 可复用，关闭回调只能删除仍指向同一对象的 weak 索引。
    void TestWeakIndexRequiresObjectIdentity() {
        auto oldConnection = std::make_shared<int>(1); // 模拟已关闭但回调尚未完成的旧对象
        auto replacement = std::make_shared<int>(2); // 模拟复用同一 fd 的新对象
        std::weak_ptr<int> registered = replacement; // fd map 当前登记的新连接

        Require(MatchesWeakIdentity(registered, replacement.get()),
            "Registered weak index should match its current object");
        Require(!MatchesWeakIdentity(registered, oldConnection.get()),
            "Stale close callback must not match a replacement object");

        replacement.reset();
        Require(!MatchesWeakIdentity(registered, oldConnection.get()),
            "Expired weak index must not match any stale callback");
    }

    // connect 完成对象只有仍为 Connected 且 fd 有效时才能登记并交付租约。
    void TestLeaseRegistrationRequiresLiveConnection() {
        Require(IsConnectionLeaseable(true, true),
            "Connected object with a valid socket should be leaseable");
        Require(!IsConnectionLeaseable(false, true),
            "Closed object must not be registered after a late close callback");
        Require(!IsConnectionLeaseable(true, false),
            "Connection without a valid socket must not enter the pool index");
    }
}

int main() {
    try {
        TestWeakIndexRequiresObjectIdentity();
        TestLeaseRegistrationRequiresLiveConnection();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; // CI 日志保留首个身份契约失败原因
        return 1;
    }
}
