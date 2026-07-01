#pragma once

namespace LikesProgram {
    namespace Net {
        enum class IOEvent : int {
            None = 0x00,
            Read = 0x01,
            Write = 0x02,
            Close = 0x04,
            Timeout = 0x08,
            Error = 0x10
        };

        // 合并多个事件位。
        constexpr IOEvent operator|(IOEvent a, IOEvent b) noexcept {
            return static_cast<IOEvent>(static_cast<int>(a) | static_cast<int>(b));
        }

        // 取两个事件集合的交集。
        constexpr IOEvent operator&(IOEvent a, IOEvent b) noexcept {
            return static_cast<IOEvent>(static_cast<int>(a) & static_cast<int>(b));
        }

        // 可被业务启停的事件位集合，Error 由轮询器就绪结果提供。
        constexpr IOEvent kIOEventMask =
            IOEvent::Read | IOEvent::Write | IOEvent::Close | IOEvent::Timeout;

        // 只在已定义事件位范围内取反，避免产生未知位。
        constexpr IOEvent operator~(IOEvent a) noexcept {
            return static_cast<IOEvent>(
                static_cast<int>(kIOEventMask) & ~static_cast<int>(a));
        }

        // 原地合并事件集合。
        constexpr IOEvent& operator|=(IOEvent& a, IOEvent b) noexcept {
            a = a | b;
            return a;
        }

        // 原地收窄事件集合。
        constexpr IOEvent& operator&=(IOEvent& a, IOEvent b) noexcept {
            a = a & b;
            return a;
        }

        // 判断事件集合中是否包含指定事件。
        constexpr bool HasEvent(IOEvent events, IOEvent event) noexcept {
            return static_cast<int>(events & event) != 0;
        }
    }
}
