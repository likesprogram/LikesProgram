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

        // 按位或
        constexpr IOEvent operator|(IOEvent a, IOEvent b) noexcept {
            return static_cast<IOEvent>(
                static_cast<int>(a) | static_cast<int>(b)
                );
        }
        // 按位与
        constexpr IOEvent operator&(IOEvent a, IOEvent b) noexcept {
            return static_cast<IOEvent>(
                static_cast<int>(a) & static_cast<int>(b)
                );
        }

        // 合法位掩码（现在是合法 constexpr）
        constexpr IOEvent IOEventMask =
            IOEvent::Read |
            IOEvent::Write |
            IOEvent::Close |
            IOEvent::Timeout;

        // 仅在合法位范围内
        constexpr IOEvent operator~(IOEvent a) noexcept {
            return static_cast<IOEvent>(
                static_cast<int>(IOEventMask) & ~static_cast<int>(a)
                );
        }
        // 复合赋值
        constexpr IOEvent& operator|=(IOEvent& a, IOEvent b) noexcept {
            a = a | b;
            return a;
        }
        constexpr IOEvent& operator&=(IOEvent& a, IOEvent b) noexcept {
            a = a & b;
            return a;
        }
    }
}
