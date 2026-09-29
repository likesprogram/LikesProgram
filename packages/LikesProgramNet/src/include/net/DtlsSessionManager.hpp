#pragma once
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include "net/DtlsSessionPolicy.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace LikesProgram {
    namespace Net {
        class Connection;
        class EventLoop;

        namespace Internal {
            class DtlsSessionManager final {
            public:
                // 绑定唯一 Connection 与其 issuer EventLoop，不接管二者生命周期。
                DtlsSessionManager(Connection* connection, EventLoop* loop);
                // 取消 session timer 并释放全部 Engine。
                ~DtlsSessionManager();

                DtlsSessionManager(const DtlsSessionManager&) = delete;
                DtlsSessionManager& operator=(const DtlsSessionManager&) = delete;

                // 发布 Start 前固定的 Factory、MTU、期限与资源上限。
                void Configure(
                    const DtlsEngineFactory& factory,
                    std::size_t maximumCiphertextDatagramBytes,
                    std::chrono::milliseconds handshakeTimeout,
                    std::chrono::milliseconds idleTimeout,
                    DtlsSessionLimits limits);
                // 为 connected UDP Client 创建唯一 session 并排队首个 flight。
                bool StartConnectedClient();
                // 把一个完整业务明文数据报交给指定 peer Engine。
                bool ConsumePlaintext(const Address& peer, Buffer&& plaintext);
                // 把一个完整或截断的 UDP ciphertext 数据报交给对应 Engine。
                void ConsumeCiphertext(Buffer& ciphertext, const Address& peer, bool truncated);
                // 请求指定 peer 进入协议关闭。
                void CloseSession(const Address& peer);
                // 停止接收新业务，并按 graceful 参数收敛全部 session。
                void Shutdown(bool graceful);
                // 回收指定 peer 已完成 ciphertext CQE 的排队成本。
                void DatagramWriteCompleted(const Address& peer, std::size_t queueCost) noexcept;
                // 返回只包含标量字段的线程安全统计快照。
                DtlsSessionStats Stats() const noexcept;

            private:
                struct DtlsPeerKey;
                struct DtlsSessionManagerImpl;

                // 校验并按固定顺序处理指定 generation Engine 的一次结果。
                bool ProcessSessionResult(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    const DtlsResult& result,
                    bool inputConsumed,
                    DtlsDatagramBatch& plaintextOutput,
                    DtlsDatagramBatch& ciphertextOutput);
                // 排队一个完整 ciphertext 数据报并累计对应 peer 成本。
                bool QueueSessionCiphertext(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    Buffer&& ciphertext);
                // 首次进入 Active 时发布统计与握手完成回调。
                bool NotifyHandshakeIfReady(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration);
                // 替换指定 session 的 Engine retransmit timer。
                bool ArmRetransmitTimer(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    std::chrono::milliseconds delay);
                // 首次启动或刷新 Active session idle timer。
                bool ArmIdleTimer(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration);
                // 处理握手总期限或 Engine timer completion，拒绝 stale generation。
                void HandleHandshakeTimeout(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration);
                void HandleRetransmitTimeout(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    std::uint64_t timerGeneration);
                void HandleIdleTimeout(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    std::uint64_t timerGeneration);
                // 为首个 Server ciphertext 延迟创建有界 session。
                bool CreateServerSession(
                    const Address& peer,
                    DtlsPeerKey& key,
                    std::uint64_t& sessionGeneration);
                // 取消、标记 Closing 并在 ciphertext 排空后移除指定 session。
                void CancelSessionTimers(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration) noexcept;
                void MarkSessionClosing(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration) noexcept;
                void FinalizeSessionIfDrained(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration) noexcept;
                // 发布 peer-local error；connected 模式可同时关闭 Connection。
                bool FailSession(
                    const DtlsPeerKey& key,
                    std::uint64_t sessionGeneration,
                    int error,
                    bool closeConnection) noexcept;

                DtlsSessionManagerImpl* m_impl = nullptr; // 隐藏每 peer Engine、timer 与原子统计
            };
        }
    }
}
