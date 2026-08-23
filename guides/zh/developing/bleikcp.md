# BLE iKCP

`libs/bleikcp` 在调用方已经建立的 BLE connection 上提供可靠、有序的 byte stream。它使用 iKCP 处理重传、顺序和流量窗口，但不负责发现设备、选择 peer、建立连接或制定重连策略。

## API Reference

[API Reference](/references/bleikcp)

`libs/bleikcp/include` 中实际参与项目构建的头文件是 BLE iKCP 的生产 Public API contract。

## 边界

BLE iKCP 负责：

- 在 GATT characteristic 上收发完整 KCP datagram。
- 管理 KCP worker、输入队列、发送队列和接收 buffer。
- 提供阻塞式 `read`、`write`、`flush` 和 `close`。
- 为 server connection 创建独立 session。
- 提供 connection、backpressure、protocol error 和 fatal error 通知。

调用方负责：

- 初始化 BLE host。
- Advertising、scan 和 peer selection。
- 建立 BLE connection。
- 完成 ATT MTU negotiation。
- 决定 reconnect 和 application protocol。

BLE 使用单一 Host PAL API，不按 peripheral/central 拆成两个 capability。BLE iKCP server 使用同一个 host 的 advertising/GATT server operation；client 使用 scan、connection 和 GATT client operation，两者可以由同一个 BLE host backend 同时支持。

## GATT Profile

默认 profile 使用：

- Service `0xFEE0`。
- TX characteristic `0xFEE1`，server 通过 notification 发送数据。
- RX characteristic `0xFEE2`，接收 write 和 write without response。

本地 BLE backend 应把 preferred ATT MTU 配置为 `517`，BLE iKCP 的业务 datagram 上限为 `512` bytes。实际 KCP MTU 取 `min(negotiated ATT MTU - 3, 512)`，因此 central 或系统 backend 只协商出更小 MTU 时必须自动降级；negotiated ATT MTU 低于 `53` 时拒绝建立 transport。每个 KCP datagram 必须完整放入一个 ATT value；BLE iKCP 不在 ATT 之上再增加一套自定义 fragmentation。

H2Loader BLE transport 两端显式使用 32-segment send/receive window，并一致启用标准 KCP congestion window；Host 与 Device 不能使用不一致的 CWND 策略。Device 输入 frame queue 为 64，TX/RX byte buffer 各为 32 KiB；Host 输入/输出 queue 各为 128。Device buffer 必须使用调用方注入的内存能力分配。修改 window 时必须同步审查 frame queue 与 byte buffer，不能通过 queue overflow 换取吞吐；Device 的 frame queue 与 byte buffer 分别可以容纳至少一个完整 window，Host queue 可以容纳四个完整 window。`libs/bleikcp` 的 target-independent default config 保持关闭 CWND，传入显式 config 的 consumer 使用 `no_congestion_control` 选择策略；H2Loader 必须显式开启 CWND。

H2Loader 作为 Peripheral 接受连接后，由独立的 `h2loader/blelink` PSRAM task 主动发起 ATT MTU exchange，并请求 15 ms connection interval 和 2M PHY。MTU changed event 驱动 BLE iKCP readiness，使不会自动协商 MTU 的 central/backend 也能建立 session；系统事件回调只记录 connection handle 并唤醒 task，不能在回调中执行可能阻塞的 PAL link-control 操作。对端或 controller 不接受 interval/PHY 请求时只记录结果，连接和 GATT 服务继续可用。

H2Loader 不使用默认 profile，而是在 server config 中固定覆盖为：

- Service `71a4b570-3ed8-53e2-aa0b-af3a6ba1721d`。
- TX `46d3a055-56fb-5170-98b0-d38fbcd2cf1e`。
- RX `8f62ad15-05f2-5b8d-a536-87d6c9bc0efe`。

H2Loader 管理服务使用 connectable Extended Advertising，并始终携带固定 Service UUID 和版本化 Service Data，不携带 local name。Host 从 Service Data 解析 board、active role 和 capabilities，再合成 `h2l.<board>` 显示名；连接后仍以 `stats` 交叉校验完整 identity。

Host baseline 使用 BLE Host PAL、`libs/bleikcp`、同一组 UUID 和同一个 iKCP conv。macOS CoreBluetooth Write Without Response 按 PAL 的 write-ready contract 流控；Linux 在真实 BLE Host PAL 可用前明确返回 unsupported。每次 GATT reconnect 都创建新的 KCP state；断线中的 command 失败且不能 replay。

## 注入的能力

`h2_bleikcp_api_t` 由调用方提供 BLE host、task、time、sync、system event 和 `h2_pal_mem_api_t`。BLE 使用统一的 `h2_pal_ble_host_api_t`，不引入独立的 server/client PAL object。

Library 创建的 stream、server、queue 和 buffer 使用调用方提供的 PAL mem API。Target backend 和 BLE stack state 不由 BLE iKCP 创建或拥有。

## Server

`h2_bleikcp_server_open()` 注册默认或调用方指定的 GATT profile，并等待可用 connection。只有 connection 已建立、ATT MTU 达到最低要求且 peer 已订阅 TX characteristic 后，才创建 KCP session 并调用 server handler。

Handler 获得 borrowed stream，可以阻塞调用 stream I/O，但不能保存、主动 close 或在 handler 返回后继续使用。Handler 返回后当前 session 结束，server 清理 connection-scoped state，并继续等待下一次 connection。

`h2_bleikcp_server_close()` 必须停止接受新 session、唤醒阻塞 I/O、等待 worker 和 handler 退出、解除 GATT callback 与 system-event subscription，再释放内部状态。

## Client

调用方完成 scan、connect 和 MTU exchange 后，将 connection handle 与 negotiated ATT MTU 传给 `h2_bleikcp_client_open()`。Client open 完成 service/characteristic discovery 和 TX subscription，然后返回 ready stream。

Client stream 只属于当前 connection。Disconnect 后新的读写和仍有排队或未确认输出的 `flush` 返回 closed；已经发布为本地 TX queue 与 KCP `waitsnd` 都为空的 `flush` 仍返回成功，除非 stream 已发布 fatal error。重新连接必须创建新的 stream，不能复用旧 connection 的 KCP state。

## I/O

- `h2_bleikcp_write()` 成功表示数据已进入本地发送路径，不表示 peer 已确认。
- `h2_bleikcp_flush()` 等待发送队列清空且 KCP segment 被确认；返回结果按 fatal error、已完成、正常断线的顺序判定，正常断线不能覆盖已经发布的完成结果。
- `h2_bleikcp_read()` 提供 byte-stream semantics，小 buffer 读取后保留剩余数据。
- BLE callback 只复制输入并唤醒 worker；KCP state 只能由 worker 操作。
- Event callback 必须快速返回，不能从 callback 中 close 当前 stream。

## 验证

Portable build 和 deterministic host test：

```sh
bazel test //libs/bleikcp:all
```

测试需要覆盖低 MTU 拒绝、server/client 建立、双向多段传输、小块读取、backpressure、disconnect、重新连接和 close cleanup。
