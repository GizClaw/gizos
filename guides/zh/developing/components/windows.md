# Windows Components

`libs/pal/providers/windows/` 是 Win32 OS backend 的 ownership root。稳定 semantic label 是 `//libs/pal/providers/windows/pal_core:pal_core`，Host Serial label 是 `//libs/pal/providers/windows/serial_host:serial_host`；它们只适用于 Windows x86_64，使用 Win32、Winsock2、IP Helper、BCrypt 和完整 wolfSSL 实现 PAL，不能依赖 POSIX、Linux、Darwin 或 Desktop implementation。

## Capability 与边界

`pal_core` 提供 Memory、Log、Time、Timer、Task、Queue、Sync、mounted Filesystem、Net、Netif 和 System Event，`serial_host` 提供 COM endpoint snapshot、bounded UART I/O、flush 和显式 DTR/RTS control。公共入口只暴露 opaque platform context 和既有 PAL API；`HANDLE`、`SOCKET`、UTF-16 path、wolfSSL object 与 native error 都保留在 provider 内部。

- Filesystem 只解析构造时复制的绝对 mount mapping；host composition 将每个当前可访问的本地 fixed/removable DOS drive root 挂载到对应的小写 portable path，例如 `C:\` 挂载为 `/c`。Filesystem 拒绝 drive-relative、UNC、device、遍历、非法 UTF-8 和 reparse escape。
- Host Serial 通过 Windows DOS device snapshot 枚举 `COM<n>`，使用 snapshot 中的 opaque `port_id` 打开独占 session；普通 open 不主动切换 DTR/RTS，read/write 和 flush 均保持有界。
- Net 使用正整数 generation token 隐藏 Winsock/TLS state；TLS 成功时原位升级并返回同一个 token，失败时原 TCP token 仍由调用方关闭。
- Resolver、socket I/O 和 TLS handshake/I/O 都使用有界等待；notification worker 在关闭时取消并排空。不同 socket 可以并发，同一 token 的 operation 由 backend 串行化。
- Netif snapshot 来自 IP Helper；IPv4 default route 按 route metric 加 interface metric 选择，再以 interface ID 打破平局。
- System Event 只在 committed default-route identity 改变后投递。普通外部 unsubscribe/deinit 在返回前排空 callback；handler 内取消订阅会立即停用 subscription，并由最后一个 in-flight callback 延迟回收，避免等待自身。

Platform 不拥有 wolfSSL 全局初始化。Composition 按 Windows Memory 与 `h2_windows_entropy` 调用引用计数的 `h2_wolfssl_init()`，再按 CoreHTTP/CoreMQTT/TLS consumer、wolfSSL、Windows platform 的顺序销毁。`h2_windows_platform_destroy()` 对 consumer-owned live object 返回 `H2_PAL_ERR_INVALID_STATE`，成功销毁时先排空 backend work，最后调用 `WSACleanup()`。

## 构建与验证

`--config=windows_x86_64` 在原生 Windows x86_64 host 上通过 `rules_cc` 的 `local_config_cc` 选择 MSVC ABI toolchain。Required CI 的 Windows Build/Test job 通过 `FIRMWARE_WINDOWS_RUNNER` 选择 Windows Server 2025 runner，顺序执行完整 compatible `//...` graph；Bazel 通过 `target_compatible_with` 选择目标，只有 opt-in 测试使用 `manual`。Graph Test 只从 Git inventory 读取一方 source，不能跟随 Windows workspace junction 进入 `bazel-*` output、external repository 或 cache tree。

Windows provider 的普通单元测试覆盖 capability lifecycle、错误、timeout、token generation 和 callback drain；`windows_pal_compile_smoke` 引用全部 vtable operation，`corehttp_coremqtt_link_smoke` 强制形成 Windows PAL、完整 wolfSSL、CoreHTTP 和 CoreMQTT 的 PE link closure。未标记的 `//projects/e2e/targets/cc_binary/pal:pal_e2e_test` 在 Linux、macOS 和 Windows 运行相同 OS-neutral host case ledger，只使用 loopback、临时目录和仓库内测试证书。

这些自动化结果只证明 exact-head 的 headless PAL contract，不证明真实 route transition、公共服务/信任根多样性、交互式 Desktop、真实 COM 设备通信、安装包或最终产品验收。Bluetooth、Wi-Fi radio control、audio/video/input 等不属于本 provider baseline。
