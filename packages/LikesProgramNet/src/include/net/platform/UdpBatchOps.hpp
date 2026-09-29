#pragma once
#include "net/UdpTransport.hpp"

// UDP 原生批处理共享契约，非原生平台返回 false 让通用层执行循环回退。

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            // 尝试使用当前平台原生接口批量接收 UDP 数据报。
            bool TryReceiveUdpBatch(
                SocketType fd,
                UdpReceiveDatagram* datagrams,
                std::size_t count,
                sockaddr_storage* lastPeer,
                SocketLength* lastPeerLength,
                IoResult* result);
            // 尝试使用当前平台原生接口批量发送 UDP 数据报。
            bool TrySendUdpBatch(
                SocketType fd,
                UdpSendDatagram* datagrams,
                std::size_t count,
                IoResult* result);
        }
    }
}
