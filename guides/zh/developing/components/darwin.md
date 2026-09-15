# Darwin Components

`libs/pal/providers/darwin/` 保存 macOS 等 Darwin host target 可复用的 OS provider。它可以依赖 PAL、portable Libraries、Apple system frameworks 和 private POSIX source-sharing target，但不能依赖 `libs/pal/providers/desktop`、`libs/pal/providers/ios`、App、project launcher 或具体产品。

## Provider ownership

`libs/pal/providers/darwin/pal_core` 当前导出：

- `h2_darwin_netif_api()`：基于 `getifaddrs` 的接口快照与 DNS 观察。
- `h2_darwin_system_event_api()`：有界订阅以及 `PF_ROUTE` 默认路由监听。
- `h2_darwin_serial_host_api()`：基于 IOKit 的 `/dev/cu.*` callout discovery。
- `h2_darwin_corebluetooth_ble(allocator, log)`：CoreBluetooth BLE Host provider。

CoreBluetooth 借用调用方提供的完整 Memory PAL API，不拥有或销毁 allocator，并通过同一 Darwin SystemEvent provider 投递事件。`NULL` allocator 或在 provider lifetime 中更换 allocator 都失败。非 Darwin target 由 project composition 显式选择 simulator 或 canonical unsupported provider，不在 Darwin package 中保留 stub。

CoreBluetooth delegate callback 运行在 provider 私有串行 queue `com.gizclaw.h2.darwin.corebluetooth` 上，而 Darwin SystemEvent provider 在 post 线程上同步调用 subscriber。因此 provider 用 `dispatch_queue_set_specific` 标记 backend queue：在该 queue 上产生的事件先复制 payload，再由独立串行 queue `com.gizclaw.h2.darwin.corebluetooth.events` 投递。Subscriber 不会运行在 backend queue 上，可以在 BLE event handler 中调用任意 `h2_pal_ble_*` API，不会在入口 `dispatch_sync` 上死锁。Backend 事件之间保持产生顺序；`start`、`stop`、scan start/stop 等在调用方线程产生的事件仍同步投递，不与尚在 event queue 中排队的 backend 事件排序。Handler 长时间阻塞只会延后后续 BLE 事件，不阻塞 backend queue。异步投递只复制 payload 本身，`h2_pal_ble_adv_set_event_t.set` 等 set identity 仍是借用指针；因此 `h2_pal_ble_adv_set_destroy()` 与 Host `stop` 先在 backend queue 上清除当前 advertising set，再把释放排到同一 event queue 之后，保证在释放前已排队的事件都已投递、subscriber 拿到的 set 指针在回调期间仍有效，且该地址不会在这些事件投递前被新 set 复用。Destroy 返回后仍可能收到该 set 的迟到事件，subscriber 应按 identity 忽略已销毁的 set，不能再对它调用 BLE API。`corebluetooth_test` 通过 test-only hook 从真实 backend queue 投递合成的 `BLE_CONNECTED`，并在 handler 中调用 `h2_pal_ble_unregister_gatt_services()` 验证可重入；再在 `BLE_ADVERTISING_STARTED` handler 被阻塞时 destroy 该 set，验证释放晚于事件投递。两者都不需要 Bluetooth 硬件或权限。

CoreBluetooth provider 不提供独立 legacy scan-response 配置；`h2_pal_ble_adv_set_set_scan_response_data()` 由完整 vtable entry 显式返回 `H2_PAL_ERR_UNSUPPORTED`，不能依赖零初始化 slot，也不能把 scan-response 内容合并进 primary advertising data。

CoreBluetooth 也不提供逐字节 primary advertising sequence 或 scan interval/window 的 controller-unit surface。`h2_pal_ble_adv_set_set_encoded_data()` 与 exact `interval_units_625us/window_units_625us` 因此都在保存数据、callback 或改变 activity state 前返回 `H2_PAL_ERR_UNSUPPORTED`；provider 不能把 dictionary-based advertising 或系统调度描述成 exact request，也不能 round 或静默降级。

`h2_pal_ble_connect()` 等待失败（timeout 等）时，provider 在 backend queue 上对该 peripheral 调用 `cancelPeripheralConnection:`，取消仍在进行的 connect request；若它已成为当前 connected peripheral，则一并清除 connected state 与 GATT client handle mapping，让下一次 connect 从干净状态开始。只有当前 connecting peripheral 的 connect 成功或失败回调可以推进连接；迟到的成功回调会再次取消该 peripheral，非当前 connected peripheral 的断开回调也会被忽略，不改变当前状态或完成其他操作。CoreBluetooth 回调不携带 attempt identity，因此同一 peripheral 重试期间收到旧 attempt 的迟到失败时，重试会返回 `H2_PAL_ERR_IO`，并由自身的 failed-wait cleanup 再次取消该 peripheral、清空状态与 pending operation，调用方仍可继续重试。Hardware-free test hook 覆盖等待失败与迟到回调路径，不等同于真实外设验收。

## Lifecycle 与依赖

SystemEvent init 创建 wake descriptor 与 joinable route-monitor thread；任何 partial failure 都回收本次已创建的资源。Init 与 route-monitor 通过 `PF_ROUTE` `RTM_GET` 查询当前 default route。内核把每个 `RTM_GET` reply 广播给所有 routing socket，socket buffer 满时直接丢弃消息，因此并发查询可能丢失本次 reply；查询在 1 s deadline 内每 100 ms 以新的 `rtm_seq` 重发请求，接受本次调用任一请求的 reply，deadline 到期才返回 `H2_PAL_ERR_TIMEOUT`。`netif_test` 用 test-only hook 丢弃前两个 reply 验证重发，并验证全部丢弃时仍在 deadline 附近返回 timeout。Deinit 唤醒并 join worker、等待 in-flight callback、清空 subscription，再关闭 descriptor。默认 route 消失、恢复或改变才发布 Netif event，重复状态不重复发布。

Host Serial discovery 读取 IOKit registry 的 product、VID、PID 与 serial metadata，只返回 callout endpoint，不为对应 `/dev/tty.*` alias 创建第二个 candidate。Descriptor、termios、bounded I/O、control-line 和 idempotent close lifecycle 来自 private `libs/pal/providers/posix/serial_host`；caller-visible identifier 保持原样。

Framework dependency 必须由 owning Bazel target 显式声明：CoreBluetooth provider 链接 CoreBluetooth/Foundation，Serial provider 链接 IOKit/CoreFoundation。所有 target 使用 macOS compatibility constraint；Darwin aggregate 的 semantic target 是 `//libs/pal/providers/darwin/pal_core:pal_core`。

## 验证

```sh
bazel test --config=macos_arm64 //libs/pal/providers/darwin/...
bazel query 'somepath(//libs/pal/providers/darwin/..., //libs/pal/providers/desktop/...)'
bazel query 'somepath(//libs/pal/providers/darwin/..., //libs/pal/providers/ios/...)'
```

两个 `somepath` query 都必须为空。PTY 与 deterministic Netif tests 验证 lifecycle，不等同于真实 USB serial 或真实 CoreBluetooth 外设验收。
