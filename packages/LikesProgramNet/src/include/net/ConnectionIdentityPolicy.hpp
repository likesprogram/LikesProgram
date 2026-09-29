#pragma once

#include <memory>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            // weak 索引只有仍指向预期对象时才允许执行删除。
            template<typename Value>
            bool MatchesWeakIdentity(
                const std::weak_ptr<Value>& registered,
                const Value* expected) noexcept {
                const std::shared_ptr<Value> snapshot = registered.lock(); // 比较期间延长当前索引对象
                return snapshot && snapshot.get() == expected;
            }

            // 连接只有状态和 socket 同时有效时才允许登记并交付租约。
            inline bool IsConnectionLeaseable(bool connected, bool socketValid) noexcept {
                return connected && socketValid;
            }
        }
    }
}
