# LikesProgramQuic

`LikesProgramQuic` 提供 QUIC 传输协议边界，CMake target 为 `LikesProgram::Quic`。它依赖 `LikesProgramNet` 提供 UDP 地址与缓冲类型，但不会把 QUIC 包处理搬进 Net，也不链接第三方 TLS 或 QUIC 库。

当前能力：

- `QuicEngine` / `QuicEngineFactory` 公开契约覆盖 TLS 1.3 与调用方选择的非空 ALPN（HTTP/3 使用默认 `h3`）、datagram 归属、timer、stream send/finish/reset、连接关闭和 path 事件。`QuicWireEngine` 是生产用 sans-I/O wire 实现；fake engine 仅作为契约测试替身，不作为互操作证据。
- RFC 9000 变长整数 codec：无分配，支持 1、2、4、8 字节编码与完整 62-bit 范围；当宿主字段允许时可显式使用非最小宽度；有界解析并报告已消费的字节数。
- 长首部解析与组装覆盖 version、packet type、connection ID、Initial token、length、不透明 packet number/payload、Retry token 与 integrity tag 边界，以及 Version Negotiation 的 version list 组帧。解析返回非拥有 span，并刻意不执行 header protection、packet protection 或 Retry integrity 校验。
- packet number 编码与重建同样是有界 helper：发送方从 outstanding 范围选择 1..4 字节截断表示，接收方围绕 next expected value 重建。解码前必须由调用方移除 header protection。
- ACK / ACK_ECN frame 边界：编解码 inclusive 确认范围、gap、ack delay 与可选 ECN 计数，不拥有包恢复、timer、loss detection 或拥塞控制。
- STREAM frame 边界：覆盖 RFC 9000 的 OFF/LEN/FIN 标志、stream id、offset、length 和非拥有 payload span；stream 调度与流控仍归调用方。
- CRYPTO frame 边界：供 TLS handshake adapter 使用，编解码 RFC 9000 的 offset、length、payload 字段，不提供 TLS 1.3 key schedule、packet protection 或握手状态机。
- RESET_STREAM / STOP_SENDING frame 边界：供取消适配器使用，暴露 stream id 与应用错误码（RESET_STREAM 另含 final size），不拥有 stream 生命周期或 HTTP 错误映射。
- CONNECTION_CLOSE frame 边界：供关闭/错误适配器使用，覆盖 transport 与 application 两类，暴露 error code、可选触发 frame type 和非拥有 reason span，不拥有 drain、deadline 或 HTTP 错误状态。
- 流控 frame 边界：MAX_DATA、MAX_STREAM_DATA、MAX_STREAMS、DATA_BLOCKED、STREAM_DATA_BLOCKED、STREAMS_BLOCKED，暴露 limit 与 stream id，不拥有连接记账、调度或背压状态。
- PATH_CHALLENGE / PATH_RESPONSE 边界：供路径校验使用，暴露固定 8 字节 probe 数据，不拥有探测 timer、地址校验、连接迁移或路由状态。
- NEW_CONNECTION_ID / RETIRE_CONNECTION_ID 边界：供 connection id 轮换使用，暴露借用的 connection id / reset token span，不拥有序列退役、token 生成或迁移策略。

生产 wire 路径把 packet/header-protection provider 与短首部 packet number 重建、stream frame、ACK、丢包/重传记账、连接与 stream credit、拥塞预算、connection id 跟踪、路径校验/迁移动作和调用方触发的超时处理组合在一起。具体 TLS 1.3 key schedule、AEAD/header protection 实现、UDP socket 和调度器仍由应用持有；这些是明确的 provider/runtime 边界，不是本包为 OpenSSL、SChannel 或 mbedTLS 预留的占位符。

HTTP/3 语义不属于本包。`HTTP3Session`、HTTP/3 frame、QPACK、request-stream 映射和统一 HTTP session 接口由 `LikesProgramHttp` 持有；公开的 HTTP/3 adapter 组合两个包，但不会把 HTTP 语义搬进 Quic，也不会把包处理搬进 Http。

## CMake

```cmake
target_link_libraries(MyApp PRIVATE LikesProgram::Quic)
```

本包要求 `LIKESPROGRAM_BUILD_NET=ON`，并通过 `LIKESPROGRAM_BUILD_QUIC=ON` 启用。
