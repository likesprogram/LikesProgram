#pragma once

#include <cstddef>

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            enum class DatagramWriteTarget {
                Invalid,
                Connected,
                ExplicitPeer
            };

            struct DatagramReceiveResult {
                std::size_t payloadBytes = 0;  // 实际交付给业务的容量内字节数
                std::size_t originalBytes = 0; // 内核报告的原始数据报长度
                bool truncated = false;        // 数据报是否超过接收容量
            };

            // 空数据报也占一个队列单位，避免零字节发送绕过背压。
            constexpr std::size_t DatagramQueueCost(std::size_t payloadBytes) noexcept {
                return payloadBytes == 0 ? 1 : payloadBytes;
            }

            // 把内核长度与接收容量收敛为稳定的数据报交付结果。
            constexpr DatagramReceiveResult InterpretDatagramReceive(
                std::size_t reportedBytes,
                std::size_t capacity,
                bool truncatedFlag) noexcept {
                return {
                    reportedBytes < capacity ? reportedBytes : capacity,
                    reportedBytes,
                    truncatedFlag || reportedBytes > capacity
                };
            }

            // 数据报发送不允许 TCP 式 partial completion。
            constexpr bool DatagramWriteCompleted(
                std::size_t completedBytes,
                std::size_t payloadBytes) noexcept {
                return completedBytes == payloadBytes;
            }

            // connected socket 不附带 msg_name，未连接 socket 必须持有有效显式 peer。
            constexpr DatagramWriteTarget ResolveDatagramWriteTarget(
                bool connected,
                bool explicitPeerValid) noexcept {
                if (explicitPeerValid) return DatagramWriteTarget::ExplicitPeer;
                return connected ? DatagramWriteTarget::Connected : DatagramWriteTarget::Invalid;
            }

            // 关闭、背压暂停或当前 multishot ENOBUFS 分派后不立即续投接收 operation。
            constexpr bool ShouldResubmitDatagramRead(
                bool closing,
                bool readEnabled,
                bool terminalEnobufs = false) noexcept {
                return !closing && readEnabled && !terminalEnobufs;
            }
        }
    }
}
