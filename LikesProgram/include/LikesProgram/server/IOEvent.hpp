#pragma once

namespace LikesProgram {
    namespace Server {
        enum class IOEvent : int {
            None = 0,
            Read = 0x01,
            Write = 0x02,
            Close = 0x04,
            Timeout = 0x08
        };

        // 按位运算支持
        inline IOEvent operator|(IOEvent a, IOEvent b) { return static_cast<IOEvent>(static_cast<int>(a) | static_cast<int>(b)); }
        inline IOEvent operator&(IOEvent a, IOEvent b) { return static_cast<IOEvent>(static_cast<int>(a) & static_cast<int>(b)); }
        inline IOEvent& operator|=(IOEvent& a, IOEvent b) { a = a | b; return a; }
        inline IOEvent& operator&=(IOEvent& a, IOEvent b) { a = a & b; return a; }
    }
}
