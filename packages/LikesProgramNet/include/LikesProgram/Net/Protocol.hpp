#pragma once

namespace LikesProgram {
    namespace Net {
        enum class TransportKind {
            Tcp,
            Udp
        };

        // 判断当前协议类型是否保留 UDP 数据报边界。
        constexpr bool IsDatagramTransport(TransportKind kind) noexcept {
            return kind == TransportKind::Udp;
        }
    }
}
