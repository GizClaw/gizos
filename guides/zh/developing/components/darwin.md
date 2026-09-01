# Darwin Components

`libs/pal/providers/darwin/` 保存 macOS 等 Darwin host target 可复用的 OS provider。它可以依赖 PAL、portable Libraries、Apple system frameworks 和 private POSIX source-sharing target，但不能依赖 `libs/pal/providers/desktop`、`libs/pal/providers/ios`、App、project launcher 或具体产品。

## Provider ownership

`libs/pal/providers/darwin/pal_core` 当前导出：

- `h2_darwin_netif_api()`：基于 `getifaddrs` 的接口快照与 DNS 观察。
- `h2_darwin_system_event_api()`：有界订阅以及 `PF_ROUTE` 默认路由监听。
- `h2_darwin_serial_host_api()`：基于 IOKit 的 `/dev/cu.*` callout discovery。
- `h2_darwin_corebluetooth_ble(allocator)`：CoreBluetooth BLE Host provider。

CoreBluetooth 借用调用方提供的完整 Memory PAL API，不拥有或销毁 allocator，并通过同一 Darwin SystemEvent provider 投递事件。`NULL` allocator 或在 provider lifetime 中更换 allocator 都失败。非 Darwin target 由 project composition 显式选择 simulator 或 canonical unsupported provider，不在 Darwin package 中保留 stub。

CoreBluetooth provider 不提供独立 legacy scan-response 配置；`h2_pal_ble_adv_set_set_scan_response_data()` 由完整 vtable entry 显式返回 `H2_PAL_ERR_UNSUPPORTED`，不能依赖零初始化 slot，也不能把 scan-response 内容合并进 primary advertising data。

CoreBluetooth 也不提供逐字节 primary advertising sequence 或 scan interval/window 的 controller-unit surface。`h2_pal_ble_adv_set_set_encoded_data()` 与 exact `interval_units_625us/window_units_625us` 因此都在保存数据、callback 或改变 activity state 前返回 `H2_PAL_ERR_UNSUPPORTED`；provider 不能把 dictionary-based advertising 或系统调度描述成 exact request，也不能 round 或静默降级。

## Lifecycle 与依赖

SystemEvent init 创建 wake descriptor 与 joinable route-monitor thread；任何 partial failure 都回收本次已创建的资源。Deinit 唤醒并 join worker、等待 in-flight callback、清空 subscription，再关闭 descriptor。默认 route 消失、恢复或改变才发布 Netif event，重复状态不重复发布。

Host Serial discovery 读取 IOKit registry 的 product、VID、PID 与 serial metadata，只返回 callout endpoint，不为对应 `/dev/tty.*` alias 创建第二个 candidate。Descriptor、termios、bounded I/O、control-line 和 idempotent close lifecycle 来自 private `libs/pal/providers/posix/serial_host`；caller-visible identifier 保持原样。

Framework dependency 必须由 owning Bazel target 显式声明：CoreBluetooth provider 链接 CoreBluetooth/Foundation，Serial provider 链接 IOKit/CoreFoundation。所有 target 使用 macOS compatibility constraint；Darwin aggregate 的 semantic target 是 `//libs/pal/providers/darwin/pal_core:pal_core`。

## 验证

```sh
bazel test --config=macos_arm64 //libs/pal/providers/darwin/...
bazel query 'somepath(//libs/pal/providers/darwin/..., //libs/pal/providers/desktop/...)'
bazel query 'somepath(//libs/pal/providers/darwin/..., //libs/pal/providers/ios/...)'
```

两个 `somepath` query 都必须为空。PTY 与 deterministic Netif tests 验证 lifecycle，不等同于真实 USB serial 或真实 CoreBluetooth 外设验收。
