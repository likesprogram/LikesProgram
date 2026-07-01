# LikesProgramNet

`LikesProgramNet` 提供基础网络能力：`Address`、`Buffer`、`EventLoop`、`Channel`、`Poller`、`Connection`、`Client`、`Server`，以及可继承的 `TcpTransport` 和 `UdpTransport`。

Net 只依赖 `LikesProgram::Core` 和系统 socket API，不链接 OpenSSL、Metrics、Threading 或 Logging。

`Server` 和 `Client` 默认使用 `TransportKind::Tcp`，也可以显式选择 `TransportKind::Udp`。普通 TCP/UDP 由 Net 内置 socket 实现；TLS/SSL 不作为独立包，也不由编译开关启用。

## 安全层扩展点

需要 TLS/SSL 或自定义安全层时，用户继承 `TcpTransport` 或 `UdpTransport`，然后通过 `ConnectionFactory` 把自定义 transport 传给 `Connection`。继承后默认仍保留普通 socket 通信能力：`ReadSome` / `WriteSome` / `ShutdownWrite` / `Close` 是统一分派入口，普通状态走 `ReadSocketSome` / `WriteSocketSome` 等 socket 钩子；只有显式升级并完成握手后，才走 `ReadSecureSome` / `WriteSecureSome` 等安全层钩子。

共享安全资源有独立的自动初始化入口。`ConnectionFactory(create, initializeSharedSecureResources)` 的第二个回调会在 `Server::Start` 或 `Client::Start` 中、任何连接创建前自动调用，并且同一个 factory 状态只调用一次。用户可在这里加载证书、创建 OpenSSL `SSL_CTX`、配置 ALPN、session cache 等跨连接共享资源；Net 不包含这些第三方类型，只保存和调用用户回调。回调返回 `false` 或抛出异常时，启动流程会停止，不会继续创建连接。

`InitializeSecureLayer` 会在 `Connection::Start` 中自动调用，但它只用于初始化本连接会话级安全层资源，不会自动进入握手态。共享资源不要放在这里重复加载，每个连接只在 `InitializeSecureLayer` 中创建或绑定自己的会话对象，例如 `SSL*`、BIO、fd 绑定、SNI 状态等，并引用 `ConnectionFactory` 初始化好的共享上下文。

进入 TLS/SSL 握手态必须由用户显式触发。连接成功后立即升级时，可以在 `Connection::OnSecureLayerReady` 或 `OnConnected` 中调用 `UpgradeCommunication`；收到指定消息后再升级时，可以在 `OnMessage` 解析到 STARTTLS 或自定义命令后调用 `UpgradeCommunication`。派生 transport 通常在 `UpgradeCommunication` 中调用 `BeginSecureHandshake`，在 `Handshake` 完成后调用 `CompleteSecureHandshake`。

这种方式允许应用接入 OpenSSL、SChannel、mbedTLS 或其他实现，同时保持 Net 包本身没有第三方 TLS 依赖，也不需要 TLS 编译开关。

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
