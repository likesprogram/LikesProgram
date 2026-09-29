# LikesProgramNet

> **状态：completion-native 重构中。** Linux TCP/TLS、UDP/DTLS、epoll fallback 与 Windows IOCP/overlapped TCP/UDP 已实现同一 `Poller` 契约；macOS/BSD kqueue 保持最终验收后的未来候选，本文中的迁移期能力不代表最终稳定承诺。

`LikesProgramNet` 提供基础网络能力：`Address`、`Buffer`、`BufferLease`、`BufferChain`、`EventLoop`、`Poller`、`Connection`、`Client`、`Server`，以及不接触 socket 的 `TlsEngine` / `DtlsEngine` 扩展契约。Transport 与 DTLS session manager 属于包内实现细节，不安装给外部用户。

Net 只依赖 `LikesProgram::Core` 和系统 socket API，不链接 OpenSSL、Metrics、Threading 或 Logging。

`Server` 和 `Client` 当前主线使用 completion-native TCP/UDP。TLS/DTLS 不作为独立第三方依赖，也不由编译开关引入；具体密码 backend 只能由包外 Engine 提供，仓库本身不链接或实现第三方库适配。QUIC 传输协议属于独立的 `LikesProgramQuic` 包。

## 高并发基座与背压

Net 的定位是网络服务与客户端基座，`Connection` 之后的协议解析、转发或业务处理由用户实现。当前已经落地的高并发基础能力包括：

- `EventLoop::PostTask` 会唤醒阻塞中的事件循环，跨线程投递任务不再依赖短轮询超时自然返回。
- `EventLoopGroup` 与 `Server::SetWorkerThreads(n)` 提供 Multi-Reactor 基础分发：主 Reactor 负责监听和 accept，连接 I/O 固定归属 worker Reactor。
- `Buffer::PrepareWrite` 允许传输层直接写入 Buffer 可写区，TCP 读路径避免 `recv -> 临时数组 -> Buffer` 的额外复制。
- `Connection::Send(Buffer&&)` 支持移动发送 Buffer，跨线程投递时避免先复制到临时 `vector`。
- `Connection::SetWriteWatermark(high, low)` 通过 completion/readiness 共用的私有原子策略提供写队列高/低水位迟滞；业务可在 `OnWriteHighWatermark` 中 `PauseReading()` 取消长期 read，在 `OnWriteLowWatermark` 中 `ResumeReading()` 重提 read。
- `Connection::SetMaxPendingWriteBytes(max)` 提供慢连接硬上限保护，超过后触发 `OnWriteQueueOverflow` 并关闭连接，避免上层持续写入导致底层无限积压内存。
- `Connection::Shutdown()` 在 API 返回前原子进入 `Closing`，立即拒绝后续应用 `Send` 和连接池复用；socket/TLS 副作用仍在 issuer 线程执行，已经进入 Poller 的 Buffer/BufferChain 按 partial completion 排空，最后一次写完成后才关闭写端。TLS 使用同一写闸门与 close_notify 顺序。
- UDP 在 io_uring 下使用 peer-aware multishot/provided-buffer receive 与 linked send completion，在 epoll 下使用 owning `Buffer` 的 level-triggered drain 与 FIFO 单数据报发送；公开发送入口始终是 `Connection::Send()` / `SendTo()`，旧私有 `UdpTransport` 批处理类型不进入安装 API。
- TCP `Client` 与 `ConnectionPool` 通过同一 Poller connect completion 建连；factory、连接挂载和启动都在 owner EventLoop 的 issuer 线程完成，调用线程不执行阻塞 `connect()`。
- `ConnectionPool` 提供 TCP outbound/client 复用子能力：`ConnectionPoolOptions` 配置远端地址、最大连接数、最大空闲连接数、worker 数和获取超时，`Acquire` / `TryAcquire` 返回 `ConnectionLease`，租约析构或 `Release` 会归还可复用连接，`Discard` 会丢弃不可复用连接。名额等待与建连共用 acquire deadline，Pool Shutdown 会取消未完成 connect；TLS Engine session 始终随物理连接保存。
- `Poller` 是所有平台唯一的底层 completion 契约；Linux 默认使用 `io_uring`，也可在同一二进制中选择 completion-compatible epoll fallback，`Connection`、Buffer ownership、TLS/DTLS Engine、取消和关闭状态机不按后端分叉。
- `CompletionStats` 通过 `receiveBundleCompletions`、`receiveBundleBuffers` 与 `maximumReceiveBundleBuffers` 区分“能力已探测”与“真实 CQE 已跨 buffer”；feature=false 或尚未发生 bundle read 时三项保持 0。
- Windows 使用 `iocp-overlapped`：TCP/UDP read、write、connect、accept、timer 都通过 typed overlapped operation 进入 IOCP；`WSAEventSelect` 只保留在明确隔离的 Channel compatibility path，不作为 built-in TCP/UDP completion。macOS/BSD kqueue 仍是最终验收后的未来候选。

连接池不是 UDP 抽象，也不会替代 completion 模型；构造 `ConnectionPool` 时显式传入 `TransportKind::Udp` 会被拒绝。

## 平台实现目录

Net 的平台实现只保留在私有源码目录，不进入安装公开头：

```text
src/platform/
  linux/                  # io_uring/EpollDriver、选择工厂、eventfd 与 SocketOps
  windows/                # IOCP/overlapped operation、ConnectEx/AcceptEx 与 Winsock SocketOps

src/include/net/platform/
  EventLoopWakeup.hpp     # EventLoop 唤醒共享契约
  SocketOps.hpp           # socket 操作共享契约
  UdpBatchOps.hpp         # UDP 原生批处理与通用回退选择契约
  linux/                  # Linux IoUringPoller、EpollDriver 与选择策略私有头
  windows/                # Windows IocpPoller、IocpOperation 与平台策略私有头
```

CMake 只收集当前目标系统对应的平台源目录。平台专用系统头、初始化、错误码、收发参数适配、原生批处理、唤醒机制和 I/O 后端不得重新散落到 `src/` 根目录；跨平台业务实现通过 `net/platform/` 下的窄接口访问底层能力。Linux epoll 与未来的 macOS/BSD kqueue 共用私有 POSIX readiness completion core，Windows IOCP 保持独立 typed completion 后端；这些平台实现不进入安装公开头，公共 Poller 行为与 backend 诊断名保持稳定。

## Linux 后端选择与诊断

默认工厂读取进程环境变量 `LIKESPROGRAM_NET_BACKEND`，只识别精确小写值：

- `auto` 或未设置：构造 io_uring 后读取一次构造期 readiness；可用时选择 io_uring，否则返回 epoll。运行期 `Activate()` 失败不会再次切换后端。
- `io_uring`：强制保留 io_uring；构造或激活不可用时由调用方看到创建/激活失败，不静默回退 epoll。
- `epoll`：强制使用 epoll completion fallback。
- 其他值：按 `auto` 处理。

稳定诊断名分别为 `io_uring-multishot-provided-buffer` 与 `epoll-level-completion`。epoll 不伪装 io_uring 能力：`CompletionStats` 中 provided-buffer、UDP multishot、active datagram lease、receive bundle 与 CQ budget 等专属字段保持 0/false；TCP/UDP、TLS/DTLS、timer、connect/accept、背压、取消和关闭的公共语义保持一致。

Windows 的稳定诊断名为 `iocp-overlapped`。IOCP 不报告 io_uring 专属能力，`CompletionStats` 中 provided-buffer、receive bundle 和 UDP multishot 字段保持 0/false；UDP 使用固定 65,535-byte wire buffer，再按业务容量交付 original length/truncated 元数据。

选择枚举、策略、工厂和两个后端类全部位于私有源码目录，不安装到 `include/LikesProgram/Net`。外部用户继续只依赖 `CreateDefaultPoller()`、`Poller::BackendName()`、`CompletionStats` 与既有 `Connection` API，无新增公共后端类型或 ABI。

启用 `LIKESPROGRAM_BUILD_EXAMPLES=ON` 时会同时构建 `Benchmark` target，源码位于 `benchmarks/Benchmark.cpp`。输出先记录实际 `backend` 诊断名，再覆盖 TCP echo、多客户端、单连接双端同时读写、代理 relay、空闲连接、慢客户端背压，UDP echo/SendTo/batch/多 peer、TLS/DTLS Engine 合同计数、`PostTask` 唤醒延迟和短时重复启停；TLS/DTLS 行只统计公开 Engine 边界调用与 ownership，不引入具体密码库或宣称密码性能。这些数据只属于 raw event/connection foundation 基线，不代表完整 HTTP 反向代理。详细字段、当前证据和下一步 relay 验收边界见 `docs/plans/NET_HIGH_CONCURRENCY_BENCHMARKS.zh-CN.md`。

## 安全层扩展点

需要 TLS/SSL 时，用户只实现不接触 socket 的 `TlsEngine`，再把每连接创建 lambda 和可选共享资源初始化 lambda 交给 `TlsEngineFactory`。`ConnectionFactory` 只负责创建业务 `Connection`，不再承担证书或 TLS 上下文初始化职责。

`TlsEngine` 通过 `NegotiatedVersion()` 报告 `Tls12`/`Tls13`，旧 Engine 默认返回 `Unknown`。QUIC 的 TLS 1.3 报告与传输契约由 `LikesProgramQuic` 负责。

连接启动前调用 `SetTlsEngineFactory()`；可用 `SetTlsHandshakeTimeout()` 调整默认 10 秒握手超时，传入 0 可禁用。直接 TLS 可在 `OnSecureLayerReady()` 中调用 `UpgradeCommunication()`，STARTTLS 可在业务协议确认升级后调用同一函数。握手进入 Active 时触发 `OnHandshakeDone()`；解密结果优先进入可覆盖的 `OnMessageChain()`，默认才适配为连续 `Buffer`；`Shutdown()` 会先发送 Engine 产生的 close_notify，密文 CQE 完成后再关闭 socket。

当前已覆盖直接 TLS、STARTTLS、ALPN Engine 查询、密文/明文双向 chain、写队列背压、close_notify 顺序和握手超时；握手 timeout 由统一 Poller timer operation 产生 completion，不扫描全部连接。

## DTLS Engine 扩展点

纯 DTLS 使用 `DtlsEngineFactory` 配置现有 UDP `Connection`。connected UDP 的 `Client` Factory 固定持有一个 session，并在 `Start()` 时自动调用 `StartHandshake()`；`Server` Factory 在收到未知 peer 的首个完整 ciphertext 数据报时按 peer 延迟创建 Engine，多个 peer 共享 socket 但隔离 Engine、timer、排队成本、错误与关闭状态。

`DtlsEngine` 只消费和产生完整数据报：每次 `ConsumeCiphertext()` / `ConsumePlaintext()` 输入对应一个 UDP datagram，`DtlsDatagramBatch` 的每个元素也分别形成一次 UDP send 或业务 `OnDatagram()` 回调。Net 不合并、拆分、重排或解释 record sequence；cookie、重传 flight、丢包、duplicate/replay、乱序与 fragment 重组属于 Engine。Engine 不接触 socket、Poller 或 provided-buffer。

默认保护值为 ciphertext UDP payload 上限 1200 字节、握手总期限 30 秒、Active 空闲期限 5 分钟、总会话 1024、Handshaking 会话 256、每 peer 待完成 ciphertext 256 KiB。`SetDtlsMaximumCiphertextDatagramBytes()` 的值只表示 UDP payload，不含 IP/UDP header；Factory 创建每个 Engine 时会收到同一精确值。重传 timer 由 Engine 通过 `ArmRetransmitTimer` 请求，Net 负责 completion 调度与 stale generation 抑制；握手和 idle deadline 由 Net 固定管理。

会话首次进入 Active 时调用 `OnDtlsHandshakeDone(peer, protocol)`；peer-local fatal error 调用 `OnDtlsSessionError(peer, error)`，该 peer 的最后一个 ciphertext CQE 回收后调用 `OnDtlsSessionClosed(peer)`。connected session 结束会同时关闭 Connection；unconnected Server 的其他 peer 与共享 socket 保持运行。安装态最小派生示例见 `tools/likesprogram-net-consumer-check/main.cpp`。

本切片不提供具体 OpenSSL、SChannel 或 mbedTLS 后端，也不实现 DTLS Connection ID、NAT rebinding、peer migration、STARTDTLS、0-RTT 或 session resumption；地址变化始终视为新 peer。

## CMake

```cmake
target_link_libraries(MyApp PRIVATE LikesProgram::Net)
```

启用包：

```powershell
cmake -S . -B build-net -DLIKESPROGRAM_BUILD_NET=ON
cmake --build build-net --config Debug
ctest --test-dir build-net --output-on-failure -C Debug
```

Windows 安装态门禁使用同一构建树：

```powershell
cmake --install build-net --config Release --prefix C:\LikesProgramInstall
likesprogram-doctor --require net
cmake -S tools/likesprogram-net-consumer-check -B build-net-consumer-check -DCMAKE_PREFIX_PATH=C:\LikesProgramInstall
cmake --build build-net-consumer-check --config Release
```

安装态 consumer 应输出 `LikesProgramNet consumer check passed`。该验收会从
`LikesProgram::Net` 聚合头检查 Buffer 写入、TCP 背压钩子、EventLoopGroup、连接池
的 UDP 拒绝、Poller 稳定 backend 名称，以及 TLS/DTLS Engine Factory 的复制和
数据报边界；它只依赖安装前缀，不读取源码树或 `src/include` 私有头。

安装态 doctor 也应在同一前缀上通过：

```powershell
$env:PATH = "C:\LikesProgramInstall\bin;$env:PATH"
likesprogram-doctor --require net --format json
```

JSON 中 `net.state` 为 `passed`，`detail` 会包含当前平台稳定 backend 名称：
Windows 为 `iocp-overlapped`，Linux 为 `io_uring-multishot-provided-buffer` 或
强制 fallback 时的 `epoll-level-completion`。这些名称用于诊断和发布门禁，应用
代码不应依赖平台私有 Poller 类型。
