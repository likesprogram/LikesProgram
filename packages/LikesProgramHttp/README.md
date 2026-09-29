# LikesProgramHttp

`LikesProgramHttp` 提供与网络传输解耦的 HTTP 协议 codec 和会话契约，模块名按现有包命名习惯确定为 `LikesProgramHttp`，CMake target 为 `LikesProgram::Http`。

当前能力：

- HTTP/1 请求/响应完整报文解析；支持唯一 `Content-Length` 或 RFC 9112 `Transfer-Encoding: chunked` 定界。
- HTTP/1 请求/响应组装支持 chunk extensions、trailer 和跨输入分片；重复/溢出长度、`Transfer-Encoding` 与 `Content-Length` 冲突、不支持的 coding、歧义定界和请求走私输入会被拒绝。
- `Http1MessageLimits` 为 header/body/trailer/chunk-line 提供有界资源策略；`Http1ChunkedDecoder` 支持增量 Feed、Finish、Reset 和尾部字段提取。
- HTTP/2 连接前言识别与组装。
- HTTP/2 二进制帧层解析与组装。
- HTTP/2 HPACK 静态/动态表、Huffman 解码、字段编码/解码、Never Indexed 保留、资源限制和 `COMPRESSION_ERROR` 映射。
- HTTP/3 标准帧解析与组装、QUIC 变长整数 codec，以及无 socket 的 control stream/SETTINGS 状态契约。
- `HttpSession` 请求发起与请求处理器契约。
- `HttpWebsite.hpp` 提供有界 URI/redirect 安全决策、cookie jar/SameSite、Range/206/416、multipart、认证挑战与 Basic/Bearer、Cache-Control/条件请求，以及调用方内容解码器 handoff。

HTTP/2/3 当前定位是 sans-I/O codec/session 加有界 DATA 正文 decoder：HTTP/2 处理连接前言、9 字节帧头、payload、stream id、SETTINGS、窗口、stream lifecycle 和 HPACK；HTTP/3 处理标准帧头、QUIC varint、QPACK、request/control stream、由适配层传入 stream-id/FIN 的 DATA 正文和 H3 error mapping。QUIC、socket、timer、scheduler 与优先级调度仍由 adapter 提供，避免把传输实现硬编码进协议层。

`Http3ControlStream` 是无 socket 的接收侧 control-stream 边界。适配器先交接 QUIC 单向 stream type `0x00`，再按完整 `Http3Frame` 交接事件；状态机会要求 SETTINGS 为首帧且只出现一次，解析去重的 QUIC varint setting pairs，拒绝 DATA/HEADERS/PUSH_PROMISE，允许未知扩展帧，并约束 GOAWAY 不递增、MAX_PUSH_ID 不递减。`BuildHttp3ControlStreamType`、`BuildHttp3Settings` 和 `ParseHttp3Settings` 只编解码字节，不创建 stream、线程或调度。`Http3RequestStream` 的 H3 error code 只表示协议状态映射，不会向 QUIC adapter 发出 RESET_STREAM/STOP_SENDING；QUIC ownership/deadline 和真实 wire action 仍由后续适配器契约负责。

独立的 `LikesProgramQuic` 提供自有 `QuicEngine`/`QuicEngineFactory` contract，负责 UDP datagram、TLS 1.3 要求、ALPN、stream、timer、关闭和路径事件的交接，并公开依赖 `LikesProgramNet` 的传输边界。该 contract 不使用第三方 TLS/QUIC 类型；HTTP 层只消费 QUIC adapter 的 stream event 并产生 stream action。真实 caller-owned TLS/UDP/aioquic interop、丢包、取消、fallback、pooling 和 path migration 证据位于产品树外；产品仍不承载具体 provider。

`HTTP3Session` 的协议语义属于 `LikesProgramHttp`：它将负责 HTTP/3 frame、QPACK、request-stream、H3 错误映射和统一 Session 语义；未来的组合适配器可以同时依赖 `LikesProgramHttp` 与 `LikesProgramQuic`，但 QUIC packet、丢包恢复、拥塞控制、流控和路径迁移不放入 HTTP 包。

`HttpSession` 的 `HttpTransport` 和 `HttpRequestHandler` 都是用户注入的非拥有接口。默认构造的 Session 不打开连接、不创建线程，也不依赖 `LikesProgram::Net`、第三方网络库或 TLS 库；应用可以自行组合 Net、curl 或自研传输适配器。

同一个 `HttpSession` 的协议选择和适配器绑定由调用方负责同步；不同 Session 以及无共享状态的 HTTP/1、HTTP/2、HTTP/3 codec 可以并发使用。用户 `HttpTransport` / `HttpRequestHandler` 回调抛出的异常会在 Session 边界转换为 `StatusCode::Internal`，回调主动返回的失败状态保持原样。

`HttpNegotiatedProtocol` 是 transport adapter 交给 Session 的连接上下文，包含
HTTP 版本、ALPN、secure/datagram 和 fallback 标记。`HttpSession::Send()` 会优先
调用带上下文的 `HttpTransport::Exchange()`；旧的 `Exchange(request, version)`
实现通过默认重载继续兼容。`SetVersion()` 仍是手动策略入口，但会清空 ALPN 和
transport 属性，不会把 `Http3` 推断成 UDP/QUIC 已可用。ALPN 文本只能由连接协商
或 adapter 显式交接，未知 ALPN 不会猜测版本。

仓库当前没有名为 `HTTPServerSession` 或 `HTTPClientSession` 的独立具体类。现代网站验收中的 server role 对应 `HttpSession(nullptr, handler)`，client role 对应 `HttpSession(transport, nullptr)`；这两个 role 只提供同步的业务回调边界，不拥有连接池、socket、线程或生命周期调度。

特别是 `HttpVersion::Http3` 不是 UDP/QUIC 实现开关。它只作为参数传入 `HttpTransport::Exchange`；HTTP/3 的 UDP socket、QUIC connection ID、TLS 1.3/ALPN、丢包恢复、拥塞控制、stream scheduling、FIN/RESET/STOP_SENDING 和 H3 error mapping 必须由外部 transport adapter 提供。`HTTPClientSession` 的现代网站就绪状态因此不能由当前 `HttpSession::Send` 单独证明。

`Http3QuicAdapter` 现在提供一个更窄的非拥有交接契约：外部 QUIC 实现交接已完成握手后的 stream data/FIN/reset/STOP_SENDING/connection-close event，HTTP 层通过 action sink 交接 stream data/FIN/reset/STOP_SENDING/connection-close action。stream ID 按 QUIC 的 62-bit varint 范围校验，包含合法的 stream 0。`MapHttp3RequestStreamError()` 将 request-stream H3 failure code 映射为调用方可分别执行的 RESET_STREAM/STOP_SENDING action plan；它不创建 QUIC action，也不决定已关闭方向。调用方可设置 Core `Time::Deadline`；adapter 只在 `CheckDeadline()`/action 边界检查并返回 `DeadlineExceeded`，不拥有 timer、线程或 QUIC close 策略。该对象只校验顺序并记录 stream 生命周期，不解析 UDP packet，不拥有 QUIC、TLS 或 socket，也不把原始 datagram 误报成 HTTP/3 wire 实现。

现代网站前置验收矩阵见 `docs/plans/HTTP_MODERN_WEBSITE_READINESS.md`。该矩阵把本地语义、外部 transport 必需能力和真实站点互操作分开计分；`HttpWebsite.hpp` 的 reusable policy primitives 已纳入证据，浏览器导航、公共后缀数据、具体 decompressor、TLS/QUIC/socket/timer/scheduler 仍明确由调用方选择。

`HTTPServerSession`/`HTTPClientSession` 在本包中仍是 `HttpSession` 的两个
role，而不是独立的具体类；两者共享 `HttpRequest` 到
`Result<HttpResponse>` 的语义接口。这个统一接口不等于统一传输：当前
`HttpSession` 不会创建 socket、线程或自动导航，调用方仍需选择版本并注入
transport。HTTP/3 浏览器选择还需要外部 UDP/QUIC、TLS 1.3 和 ALPN；包内提供
有界 Alt-Svc/回退/replay 与 pool/migration 账本，不会把 caller-owned provider
冒充为 Http 实现。Cookie、redirect、Range、multipart、认证、缓存和内容编码的
reusable 语义由 `HttpWebsite.hpp` 提供，应用仍决定凭据、导航、压缩 provider 和
缓存部署策略。

HTTP/1 response parsing/building accepts an explicit `Http1ResponseContext` for HEAD and CONNECT. 1xx/204/304 responses are terminated at the header section, while hypothetical HEAD/304 framing is preserved without consuming response bytes; successful CONNECT responses ignore received framing fields and builders reject unsafe tunnel framing.

`Http1Connection` is the sans-I/O pipeline boundary. Feed bytes into a request or response reader, call `NextRequest`/`NextResponse` to consume one complete message while retaining residual pipeline bytes, and call `Finish` for close-delimited responses. `Http1ConnectionState` exposes close, Upgrade, and CONNECT tunnel handoff; `Drain` closes a rejected/closing HTTP stream after its residual bytes are discarded, while `TakeBufferedBytes` transfers post-handshake bytes to the owning adapter. `EvaluateHttp1Connection` and `ClassifyHttp1RequestTarget` provide keep-alive, proxy target, Upgrade, and tunnel decisions without owning a socket.

Decoded HTTP/2 and HTTP/3 header blocks can be checked with
`ValidateHttp2HeaderBlock` and `ValidateHttp3HeaderBlock`. `Http2HpackCodec`
can decode and validate an HTTP/2 block in one bounded call while preserving
Never Indexed metadata through `DecodeFields`. The validator enforces
lowercase names, pseudo-header ordering and direction, CONNECT and extended
CONNECT requirements, trailer restrictions, and forbidden connection-specific
fields. HTTP/3 QPACK remains a separate stateful codec; neither compression
codec owns a socket, stream scheduler or transport callback.

`Http2Session` is the corresponding sans-I/O connection/state boundary. It
tracks SETTINGS ACKs, local and peer stream-id parity, HEADERS/CONTINUATION
ordering, OPEN/HALF-CLOSED/CLOSED transitions, GOAWAY and RST_STREAM, and
connection plus per-stream send/receive windows. `SendData` and
`ReceiveData` consume byte counts supplied by an adapter; `ApplyWindowUpdate`
and `ConsumeReceivedData` expose credit changes without owning a socket,
HPACK decoder, or QUIC transport.

`HttpBodySink` is a bounded sans-I/O pull handoff for adapters that need to
separate transport reads from application consumption. `Push` accepts only
the remaining queue capacity and returns the accepted byte count; `Pull`
releases bytes to the consumer. High/low watermarks are exposed through
`NeedsPause` and `CanResume`, while `Pause`/`Resume`, `Close`, and `Cancel`
define explicit ownership transitions. `SetDeadline` and `SetIdleTimeout`
share Core's monotonic clock contract; timeout checks clear queued data and
return `StatusCode::DeadlineExceeded`. The sink does not start threads or
perform transport callbacks, so adapters remain responsible for applying
backpressure to their socket, QUIC stream, or producer.

`HttpBodyProducer` is the outbound counterpart: the application calls `Push`
and the transport adapter calls `Pull`. It reuses the same bounded queue,
watermark, pause/resume, close/cancel/reset, and Core-clock timeout semantics;
`Close` stops new application writes while allowing the adapter to drain
already queued bytes. The producer is non-owning and sans-I/O, so it does not
write to a socket, schedule work, or implement HTTP/2/3 flow-control frames.

`HttpBodyBudget` provides optional shared queued-byte accounting for a logical
connection. Attach the same non-owning budget to each sink or producer with a
stream id; reservations are bounded by both `maxConnectionBytes` and
`maxStreamBytes`, and are released by `Pull`, `Cancel`, `Reset`, timeout, or
destruction. `Reserve` returns partial acceptance so adapters can apply normal
backpressure. Budget access is adapter-serialized, and `Reset` is rejected
while any stream still holds a reservation.

The two-argument budget constructor accepts independent connection and stream
low/high watermarks without changing the legacy limits layout. A non-owning
`HttpBodyBudgetObserver` receives one connection or stream pause notification
when a high watermark is crossed and one resume notification after usage falls
to the matching low watermark. Observer callbacks are serialized with budget
access and must not re-enter the budget. The legacy constructor resolves high
watermarks to the hard limits and low watermarks to one byte below them. A
successful `Reset` retains the observer attachment, and moving the budget
transfers that attachment with the budget state.

`HttpBodyCancellation` is the non-owning cancellation bridge shared by a body
handoff and its transport adapter. `Cancel` carries an explicit reason
(`Application`, `PeerReset`, `DeadlineExceeded`, or `ProtocolError`) and
invokes the adapter once; `Reset` reopens the state and invokes the matching
adapter reset action. Attaching a different adapter or attaching after a
terminal action is rejected. The bridge only defines the lifecycle callback
contract; it does not create threads or emit socket/QUIC frames.

`HttpBodyBackpressureAdapter` is the corresponding non-owning watermark
boundary. After a sink or producer reaches its high watermark, or a shared
queued-byte budget has no capacity, it emits one `Pause()` notification. Once
the queue falls to its low watermark with capacity available, it emits one
`Resume()` notification. The callbacks are adapter-owned actions: they do not
start scheduling, touch a socket, or replace HTTP/2/3 flow-control frames.
`Reset()` clears a pending pause and emits the matching resume so a reusable
adapter cannot remain paused; cancellation and expiry retain their terminal
semantics.

`HttpBodyBudget::Snapshot()` returns a value snapshot of the configured
connection/stream byte limits, currently reserved queued bytes, and the number
of streams holding reservations. The snapshot is observational only and does
not add synchronization or change the adapter-serialized reserve/release
contract; header compression tables, protocol windows, and full connection
resource accounting remain outside this body budget.

`Http2Session::Snapshot()` provides the corresponding read-only view of the
 sans-I/O HTTP/2 state machine: local and peer window/frame/concurrency limits,
 connection windows, active local/remote streams, pending CONTINUATION and
 SETTINGS ACK state, GOAWAY boundaries, and the terminal error code. It is a
 point-in-time diagnostic value and does not add HPACK/QPACK, socket, QUIC, or
 transport synchronization.

`Http1Connection::Snapshot()` exposes the HTTP/1 reader kind, lifecycle state,
configured limits, buffered bytes, and EOF state. `HttpErrorContext` is the
cross-version diagnostic coordinate returned alongside the existing retained
`Status`: it identifies HTTP version, connection or stream scope, protocol,
application, resource, lifecycle, or transport origin, stream id, optional
HTTP/2 frame or HTTP/3 event type, and the HTTP/1 byte offset when applicable.
`Http1Connection`, `Http2Session`, and `Http3QuicAdapter` expose this same
read-only shape without replacing their richer protocol-specific snapshots.
The owning object is the connection identity; the context does not create a
global connection registry, logging backend, socket, timer, scheduler, QUIC
state, or concrete TLS provider.

The HTTP/1 chunked decoder and HTTP/2/HTTP/3 DATA decoders can attach a
non-owning `HttpBodySink` before body data arrives. H2/H3 reject a complete
DATA frame when the sink has insufficient capacity; the HTTP/1 decoder keeps
the already-fed chunk bytes pending and resumes when the caller drains or
resumes the sink and calls `Feed({})`. END_STREAM/FIN or the final chunk
trailers close the sink, while decoder `Cancel` and `Reset` propagate to it.
These APIs remain sans-I/O; transport adapters still own scheduling, socket or
QUIC flow control, producer coordination, and wire-level cancellation.

## CMake

```cmake
target_link_libraries(MyApp PRIVATE LikesProgram::Http)
```

启用包：

```powershell
cmake -S . -B build-http -DLIKESPROGRAM_BUILD_HTTP=ON
cmake --build build-http --config Debug
ctest --test-dir build-http --output-on-failure -C Debug
```

### Incremental HTTP/2/3 stream bodies

`Http2StreamBodyDecoder` and `Http3StreamBodyDecoder` accumulate DATA payloads
one frame at a time without owning a socket or QUIC connection. Each instance
is bound to one stream id and has a configurable `maxBodyBytes` limit (8 MiB
by default). HTTP/2 reads `END_STREAM` from DATA flags; HTTP/3 receives the
QUIC stream id and FIN flag from the transport adapter because HTTP/3 frame
headers do not carry a stream id. `Finish()` requires the end marker,
`Cancel()` stops the instance, and `Reset()` clears the body for reuse. A
decoder rejects wrong stream ids, non-DATA frames, inconsistent HTTP/2 lengths,
frames after completion, and bodies over the configured limit. Separate
decoder instances can be fed concurrently by their owning transport tasks.

启用 examples 时会同时构建 `LikesProgramHttpBenchmark`。必须使用 Release 运行；输出分别记录 HTTP/1 完整报文、HTTP/2 frame、HTTP/3 frame 和 `HttpSession` 的固定工作负载吞吐、`ns/op` 与 MiB/s。`std::string` / `std::vector` 复制和直接 Transport 调用只作为内存与分发下界，不代表 Boost.Beast、nghttp2、Nginx 或其他完整 HTTP 实现的公平竞品结论。

Windows 可使用 `tools/run-http-codec-benchmark.ps1 -Benchmark <Release benchmark path> -Repeats 5` 创建时间戳结果目录；目录包含每轮原始日志、CSV、环境信息、吞吐/稳定性/资源 SVG 和 `index.html` 总览。Linux/macOS 可直接重复运行同一 benchmark，并按相同字段归档结果。

`LikesProgramHttp` 只依赖 `LikesProgram::Core`，不依赖 `LikesProgram::Net`、Logging、Config、Metrics、Threading、QUIC/TLS 或第三方 HTTP/2/HPACK 库。

## Industrial acceptance

The 2026-07-27 final acceptance at product HEAD `65ee990` combines fresh
shared/static/ASan CTest, repeated installed consumers, current codec/session
performance, independent H1/H2/H3 wire fixtures, cancellation/pooling/path
migration, and bounded modern-site semantics. The compact evidence index is
`validation-snapshots/http-final-acceptance-65ee990.tgz` with SHA-256
`DF543B9E7CC31DD3A6CCB27BB2BF20FC4FCCFCB3A50CC58575B31D959B0DF71F`.
This acceptance covers the package and its caller-owned adapter contract; it
does not add or claim a concrete network, TLS, QUIC, scheduler, timer or
compression provider.
