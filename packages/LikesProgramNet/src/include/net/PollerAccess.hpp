#pragma once

#include <cstddef>

namespace LikesProgram {
    namespace Net {
        class Address;
        class Buffer;
        class BufferLease;
        class Connection;

        namespace Internal {
            struct PollerAccess final {
                // 把 socket completion 的租约交给 Connection 数据链。
                static void CompleteRead(Connection& connection, BufferLease&& lease);
                // 把拥有型 socket completion 交给同一 Connection 数据链。
                static void CompleteRead(Connection& connection, Buffer&& input);
                // 把 UDP completion 的 payload、peer 与截断信息交给 Connection。
                static void CompleteDatagram(
                    Connection& connection,
                    Buffer& input,
                    const Address& peer,
                    std::size_t originalBytes,
                    bool truncated);
                // 把一个 UDP 写节点的原始 peer 与排队成本交给协议层回收。
                static void DatagramWriteCompleted(
                    Connection& connection,
                    const Address& peer,
                    std::size_t queueCost) noexcept;
                // 把对端 EOF 交给统一关闭流程。
                static void PeerClosed(Connection& connection);
                // 返回单个 UDP 数据报的配置接收容量。
                static std::size_t MaxDatagramBytes(const Connection& connection) noexcept;
                // 查询背压和关闭状态是否允许继续投递 read。
                static bool ReadEnabled(const Connection& connection) noexcept;
                // 写队列增长后执行统一水位和硬上限保护。
                static bool WriteGrowth(Connection& connection, std::size_t pendingBytes);
                // partial write 后按剩余字节解除低水位背压。
                static void WriteDrain(Connection& connection, std::size_t pendingBytes);
                // 写链排空后触发完成与优雅关闭语义。
                static void WriteComplete(Connection& connection);
                // 平台 completion 错误进入统一 OnError/DoClose 路径。
                static void Error(Connection& connection, int error);
                // Poller 析构前解除连接回调，避免向失效后端提交 cancel。
                static void Detach(Connection& connection) noexcept;
            };
        }
    }
}
