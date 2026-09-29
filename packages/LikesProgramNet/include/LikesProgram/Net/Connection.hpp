#pragma once
#include <LikesProgram/Net/system/LikesProgramNetExport.hpp>
#include <LikesProgram/Net/Address.hpp>
#include <LikesProgram/Net/Buffer.hpp>
#include <LikesProgram/Net/Channel.hpp>
#include <LikesProgram/Net/DtlsEngine.hpp>
#include <LikesProgram/Net/Protocol.hpp>
#include <chrono>
#include <functional>
#include <memory>

namespace LikesProgram {
    namespace Net {
        class EventLoop;
        class BufferChain;
        class DtlsEngineFactory;
        class TlsEngineFactory;
        struct TlsResult;
        namespace Internal {
            class DtlsSessionManager;
            struct PollerAccess;
        }

        class LIKESPROGRAM_NET_API Connection {
        public:
            using Task = std::function<void()>;
            using CloseCallback = std::function<void(Connection&)>;

            enum class State {
                Connected,
                Closing,
                Closed
            };

            // 接管明文 TCP socket，并由 completion Poller 统一处理 I/O 与关闭。
            Connection(SocketType fd, EventLoop* loop);
            // 按协议接管 socket；TCP 使用 completion，UDP 暂由包内兼容实现承载。
            Connection(SocketType fd, EventLoop* loop, TransportKind kind);
            // 析构时强制关闭连接。
            virtual ~Connection();

            Connection(const Connection&) = delete;
            Connection& operator=(const Connection&) = delete;

            // 注册 Channel 并触发 OnConnected。
            void Start();
            // 返回底层 socket。
            SocketType GetSocket() const noexcept;
            // 返回当前连接状态。
            State GetState() const noexcept;
            // 返回连接是否仍处于 Connected 状态。
            bool IsConnected() const noexcept;
            // 返回当前连接传输族。
            TransportKind GetTransportKind() const noexcept;
            // 设置框架内部关闭回调。
            void SetFrameworkCloseCallback(CloseCallback callback);
            // 设置每连接 TLS Engine 工厂；必须在 UpgradeCommunication 前调用。
            void SetTlsEngineFactory(const TlsEngineFactory& factory);
            // 返回当前连接是否配置了 TLS Engine 工厂。
            bool HasTlsEngineFactory() const noexcept;
            // 设置 TLS 握手超时；0 表示禁用，必须在启动安全层前调用。
            void SetTlsHandshakeTimeout(std::chrono::milliseconds timeout);
            // 设置 UDP DTLS Engine Factory；TCP 配置会抛出 invalid_argument。
            void SetDtlsEngineFactory(const DtlsEngineFactory& factory);
            // 返回当前 UDP Connection 是否配置了 DTLS Factory。
            bool HasDtlsEngineFactory() const noexcept;
            // 设置 Engine ciphertext UDP payload 精确上限；必须在 Start 前调用。
            void SetDtlsMaximumCiphertextDatagramBytes(std::size_t maxBytes);
            // 设置 DTLS 握手总期限；0 表示禁用，必须在 Start 前调用。
            void SetDtlsHandshakeTimeout(std::chrono::milliseconds timeout);
            // 设置 Active DTLS 会话空闲期限；0 表示禁用，必须在 Start 前调用。
            void SetDtlsSessionIdleTimeout(std::chrono::milliseconds timeout);
            // 设置总会话、pending 会话和单 peer 待完成密文成本上限。
            void SetDtlsSessionLimits(
                std::size_t maxSessions,
                std::size_t maxPendingSessions,
                std::size_t maxPendingCiphertextBytesPerSession);
            // 请求关闭指定 UDP peer 的 DTLS 会话；未知 peer 安全忽略。
            void CloseDtlsSession(const Address& peer);
            // 返回可跨线程读取的 DTLS 会话统计快照。
            DtlsSessionStats GetDtlsSessionStats() const noexcept;
            // 配置单个 UDP 数据报最大接收容量；必须在 Start 前调用。
            void SetMaxDatagramBytes(std::size_t maxBytes);

            // 发送 Buffer 的可读区域。
            void Send(const Buffer& buffer);
            // 移动发送 Buffer 的可读区域，跨线程投递时避免先复制到临时 vector。
            void Send(Buffer&& buffer);
            // 发送一段字节。
            void Send(const void* data, std::size_t len);
            // 向显式 UDP peer 移动发送一个完整数据报。
            void SendTo(const Address& peer, Buffer&& buffer);
            // 向显式 UDP peer 发送一个完整字节数据报，len 可为 0。
            void SendTo(const Address& peer, const void* data, std::size_t len);
            // 配置写队列高/低水位；high 为 0 时关闭水位通知。
            void SetWriteWatermark(std::size_t highWatermarkBytes, std::size_t lowWatermarkBytes = 0);
            // 配置写队列硬上限；超过后触发 OnWriteQueueOverflow 并关闭慢连接，0 表示不启用。
            void SetMaxPendingWriteBytes(std::size_t maxBytes);
            // 暂停普通业务读事件，通常由高水位回调用于向上游背压。
            void PauseReading();
            // 恢复普通业务读事件，通常由低水位回调用于解除背压。
            void ResumeReading();
            // 在 loop 线程中执行任务。
            void RunInLoop(Task task);
            // 将任务投递到 loop 线程。
            void QueueInLoop(Task task);
            // 立即进入 Closing 并拒绝新应用写，待已接受发送缓冲清空后关闭写端。
            void Shutdown();
            // 强制关闭并丢弃未发送数据。
            void ForceClose();
            // 启动 TlsEngine 握手；可用于直接 TLS 或 STARTTLS。
            void UpgradeCommunication();

            // Reactor 读事件入口。
            void HandleRead();
            // Reactor 写事件入口。
            void HandleWrite();
            // Reactor 关闭事件入口。
            void HandleClose();
            // Reactor 错误事件入口。
            void HandleError();

            // 返回对端地址缓存。
            const Address& GetRemoteAddress() const noexcept;
            // 返回本端地址缓存。
            const Address& GetLocalAddress() const noexcept;

        protected:
            // 连接建立后调用。
            virtual void OnConnected() {}
            // 安全层会话初始化完成后调用；默认不进入握手，用户可在此调用 UpgradeCommunication。
            virtual void OnSecureLayerReady() {}
            // 安全层派生传输握手完成后调用；普通 TCP/UDP 不触发。
            virtual void OnHandshakeDone() {}
            // 收到数据后调用，业务应消费已处理字节。
            virtual void OnMessage(Buffer& in) { (void)in; }
            // 收到一个完整或截断的 UDP 数据报后调用；零长度数据报也调用一次。
            virtual void OnDatagram(
                Buffer& input,
                const Address& peer,
                std::size_t originalBytes,
                bool truncated);
            // 指定 peer 的 DTLS Engine 首次进入 Active 后调用一次。
            virtual void OnDtlsHandshakeDone(
                const Address& peer,
                const char* negotiatedProtocol) {
                (void)peer;
                (void)negotiatedProtocol;
            }
            // 指定 peer 的 DTLS 会话发生 fatal error 时调用。
            virtual void OnDtlsSessionError(const Address& peer, int error) {
                (void)peer;
                (void)error;
            }
            // 指定 peer 的 DTLS 会话完成清理后调用一次。
            virtual void OnDtlsSessionClosed(const Address& peer) { (void)peer; }
            // TLS 解密后调用，默认适配到连续 Buffer；业务可覆盖后直接消费多段明文链。
            virtual void OnMessageChain(BufferChain& in);
            // 写队列达到高水位时调用，业务可暂停上游读取或拒绝继续写入。
            virtual void OnWriteHighWatermark(std::size_t pendingBytes) { (void)pendingBytes; }
            // 写队列回落到低水位时调用，业务可恢复上游读取。
            virtual void OnWriteLowWatermark(std::size_t pendingBytes) { (void)pendingBytes; }
            // 写队列超过硬上限时调用，默认随后关闭连接。
            virtual void OnWriteQueueOverflow(std::size_t pendingBytes) { (void)pendingBytes; }
            // 发送缓冲清空后调用。
            virtual void OnWriteComplete() {}
            // 关闭前调用。
            virtual void OnClosing() {}
            // 关闭完成后调用。
            virtual void OnClosed() {}
            // I/O 错误时调用。
            virtual void OnError(int error) { (void)error; }

        private:
            friend class Internal::DtlsSessionManager;
            friend struct Internal::PollerAccess;

            struct ConnectionImpl;

            // 返回 Poller 直接 I/O 使用的 UDP 业务接收容量。
            std::size_t GetMaxDatagramBytesForPoller() const noexcept;

            // built-in TCP/UDP 满足条件时切换到 Poller 直接 I/O。
            bool TryStartPollerIo();
            // 判断当前调用者是否就是执行 Start 回调的线程。
            bool IsInStartingCallbackThread() const noexcept;
            // 接管 provided-buffer lease 并触发业务回调。
            void HandlePollerRead(BufferLease&& lease);
            // 接管普通拥有型 completion 输入。
            void HandlePollerRead(Buffer&& input);
            // 统一处理 io_uring lease 与 epoll owning Buffer。
            void HandlePollerInput(Buffer&& input);
            // 接管一个 UDP 数据报并同步交付 peer 与截断元数据。
            void HandlePollerDatagram(
                Buffer& input,
                const Address& peer,
                std::size_t originalBytes,
                bool truncated);
            // 回收一个 UDP ciphertext 节点的 per-peer 排队成本。
            void HandlePollerDatagramWriteCompleted(
                const Address& peer,
                std::size_t queueCost) noexcept;
            // 消费一次对端 EOF。
            void HandlePollerPeerClosed();
            // 判断 Poller 是否应继续投递业务读。
            bool PollerReadEnabled() const noexcept;
            // Poller 写队列增长后执行背压保护。
            bool HandlePollerWriteGrowth(std::size_t pendingBytes);
            // Poller 写队列回落后解除背压。
            void HandlePollerWriteDrain(std::size_t pendingBytes);
            // Poller 写队列清空后触发统一回调。
            void HandlePollerWriteComplete();
            // Poller 操作失败时进入统一错误与关闭流程。
            void HandlePollerError(int error);
            // Poller 自身析构前解除回调关系，避免连接析构再次提交 cancel。
            void HandlePollerDetached() noexcept;
            // 统一处理 Engine 输出、握手状态与关闭动作。
            bool HandleTlsResult(
                const TlsResult& result,
                BufferChain& plaintextOutput,
                BufferChain& ciphertextOutput);
            // Engine 首次进入 Active 后触发握手完成回调。
            void NotifyTlsHandshakeIfReady();
            // 为 Handshaking 状态提交一次 Poller 定时 completion。
            void ArmTlsHandshakeTimeout();
            // 取消当前握手定时 completion。
            void CancelTlsHandshakeTimeout() noexcept;
            // 处理握手定时 CQE；仍未完成时报告 ETIMEDOUT 并关闭。
            void HandleTlsHandshakeTimeout();
            // 在 loop 线程中转交或追加待发送 Buffer；已接受任务可跨越 graceful Closing。
            void SendInLoop(Buffer&& buffer, bool acceptedBeforeShutdown = false);
            // 在 loop 线程按 connected 或显式 peer 语义转交一个完整 UDP 数据报。
            void SendDatagramInLoop(
                const Address& peer,
                Buffer&& buffer,
                bool acceptedBeforeShutdown = false);
            // 按 connected 或显式 peer 语义接受并投递一个完整 UDP 数据报。
            void SendDatagram(const Address& peer, Buffer&& buffer);
            // 跨线程发送任务完成后推进延迟的 graceful shutdown。
            void FinishQueuedSend();
            // 已接受发送任务全部进入写链后启动 TLS close_notify 或 socket 半关闭。
            void ShutdownInLoop();
            // 推进安全层派生传输握手，普通 TCP/UDP 直接返回 true。
            bool AdvanceHandshake();
            // 根据传输层握手需求刷新 Channel 关注事件。
            void RefreshTransportInterest();
            // 根据发送缓冲状态启用写事件。
            void EnableWritingIfNeeded();
            // 写队列增长后执行水位通知和硬上限保护，返回连接是否仍可继续。
            bool ApplyWriteQueueGrowthBackpressure();
            // 指定 completion 队列长度执行同一套水位保护。
            bool ApplyWriteQueueGrowthBackpressure(std::size_t pendingBytes);
            // 写队列排空或回落后解除高水位通知状态。
            void ApplyWriteQueueDrainBackpressure();
            // 指定 completion 队列长度解除同一套水位状态。
            void ApplyWriteQueueDrainBackpressure(std::size_t pendingBytes);
            // 执行统一关闭流程。
            void DoClose(bool notifyFramework);
            ConnectionImpl* m_impl = nullptr;                  // 连接实现，隐藏 transport/buffer/callback 状态
        };
    }
}
