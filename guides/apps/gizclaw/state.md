# GizClaw 状态与请求

GizClaw 集成使用 app-owned state 投影调用方能够确认的连接事实和异步请求结果。它不是一个 LVGL subject。当前 GizClaw Public API 没有公开 connection、workspace、conversation 或 OTA state enum，因此产品文档不能把调用方自定义阶段写成 GizClaw SDK 状态。

## 数据流

```mermaid
flowchart LR
    Action["App action"] --> Transition["App transition"]
    Transition --> Command["GizClaw effect command"]
    Command --> Submit["service submit"]
    Submit --> Receive["bounded request queue"]
    Receive --> Net["$gizclaw/net"]
    Net --> Poll["client poll / response copy"]
    Poll --> Completion["bounded response queue"]
    Completion --> MainLoop["App main-loop dispatch callback"]
    MainLoop --> Transition
    Transition --> State["GizClaw app-owned state"]
    State --> Subject["页面局部 LVGL subject"]
    Subject --> UI["LVGL projection"]
```

`h2_gizclaw_service_submit()` 把 typed operation context 交给 library-owned bounded queue。`$gizclaw/net` 唯一持有 client，执行网络 operation 并持续调用 `h2_gizclaw_client_poll()`；它复制 response、stream frame 和 Peer Event 后写入有界 response queue，不直接调用用户 callback。App main loop 调用 `h2_gizclaw_service_poll()`，按接收顺序执行 progress、stream、completion 和 connection event callback。队列达到容量时网络层实施背压，不会无限复制 frame。

## 当前公开状态边界

| Public API | 调用方能够确认的事实 |
| --- | --- |
| `h2_gizclaw_client_init()` | Client object 是否成功创建 |
| `h2_gizclaw_client_connect()` | 本次同步连接调用成功或失败 |
| `h2_gizclaw_client_poll()` | 本次 poll 是否成功、超时或失败 |
| `h2_gizclaw_client_rpc_call()` / `rpc_call_stream()` | 调用任意 protobuf-encoded unary 或 server-streaming RPC，并获得 response/error/data event |
| `h2_gizclaw_rpc_provider_fn` | 在 `poll()` 所在线程处理 Server 主动调用的 `client.info.get`、`client.identifiers.get` 和 `client.tool.invoke` |
| `h2_gizclaw_client_ping_measure()` | Ping RPC 的 server time 与 monotonic round-trip time |
| `h2_gizclaw_client_speedtest_download()` | 实际接收字节数、elapsed time 与下载 bit rate |
| `h2_gizclaw_client_close()` / `deinit()` | 调用方已经请求关闭并释放 client |

Generic RPC API 接收 wire method number 和 protobuf payload，因此 client surface 不需要为每个 generated method 复制一层易漂移 wrapper。Payload message、conversation state、workspace state 和 Audio frame 仍由对应 integration 持有。消费 App 可以保存自己的 request generation、pending、error 和 UI projection，但这些字段必须标记为 App/integration-owned，不能使用 `GizClaw state` 名义暗示 SDK 已提供相同合同。

Server 主动调用 Client 时，C SDK 只负责 request framing、method dispatch、response/error framing 和 channel 生命周期。Provider callback 在 `poll()` 所在线程同步执行，并在返回成功前生成且提交唯一一次 protobuf-encoded response。Request view 只在 callback 期间有效；response 与 error view 只需保持到 callback 返回，integration 不能把这些 borrowed buffer 交给异步任务后再响应。设备信息、硬件 identifiers 与本地 Tool 的真实实现仍由产品 integration 提供。Tiga H106 launcher 当前返回 board/model 和基于 eFuse MAC 的稳定 SN；尚未注册本地 Tool 时，`client.tool.invoke` 明确返回 method-not-found，不能伪造执行成功。

调用方状态通常还需要保存以下稳定数据：

| 字段 | 合同 |
| --- | --- |
| `active_workspace_name` | 保存 Peer-scoped Workspace name，不从显示名、icon 或 canonical ID 反推 |
| `request_generation` | 每次启动、取消或替换异步请求时递增 |
| `last_error` | 记录所属 domain、稳定错误码和可显示摘要 |
| `retry_count` / `retry_deadline` | 只由 effect policy 更新，不由 UI timer 猜测 |
| `firmware_channel` / `firmware_sha256` / `firmware_size` | 标识当前 channel 解析出的 OTA package，下载期间保持不变；不保存 admin firmware name 或短期 URL |

## Request 合同

每个 command 至少携带 operation、generation，以及该 API 要求的 typed resource/record name。Transition 在 command 发出前先写入 pending state；网络 task 完成后把同一 identity/generation、terminal kind 和 result 放入 response queue。只有 main-loop dispatch callback 中的 generation 与当前 state 匹配时才能提交结果。字段必须保留具体语义，例如 `workspace_name`、`history_id` 或 `firmware_channel`，不能混装成通用 `resource_id`。

取消操作先使当前 App request generation 失效，再调用 operation cancel。取消是幂等的；queued、running 或 progress-pending operation 仍由 service 持有，最终恰好产生一次 completion callback。Progress-pending 取消会唤醒 worker，已经排队但尚未执行的 progress callback 不再接触产品资源。连接断开时 service 将受影响 operation 标记为 `SERVICE_CLOSED`，App callback 再根据 typed operation 决定失败或恢复行为，不能把未由 GizClaw API 返回的 connection phase 当作 SDK 事实。

配置 Log PAL 后，service、Conversation 与 Speech request 会输出 compact lifecycle
记录：`request`、`stage`、`identity`、`rc`、`detail`、`frames` 和 `bytes`。这些字段用于
定位 queue、RPC、transport、cancel 和 dispatch 边界，不定义新的产品状态，也不能替代
terminal callback。调用者任务和 `$gizclaw/net` 共享的统计计数使用原子访问；日志不得
为诊断引入跨任务 data race。

## Subject 投影

页面只创建自己需要的 subject，例如 `chat_phase`、`connection_badge` 或 `ota_progress`。长期 GizClaw state 仍由 App 持有；切换页面时销毁页面 observer 和局部 subject，不销毁 client connection。

Subject 更新只发生在 LVGL 所属 main loop。一次性动作，如开始录音、取消下载或重启进入 H2Loader，必须保持 effect command，不能编码为 subject 的瞬时值。

## 生命周期

App 初始化依次建立 client config、service 和 app-owned state，再启动 service。退出时先停止接收新 command，取消 domain operation 和 conversation/OTA effect，调用 service stop 等待 `$gizclaw/net` 退出，再继续 dispatch，直到所有 completion callback 已 drain 并释放 caller operation handle；随后 App deinit service，最后释放 App state。Partial initialization 失败时只清理已经成功创建的资源。

## 验收

- App-owned connection request、workspace request、conversation 和 firmware operation 可以独立表达，不互相覆盖，也不冒充 GizClaw SDK enum。
- 所有 GizClaw callback 都由 App main loop 调用 service dispatch 后执行。
- 页面退出后的迟到 result 因 generation 不匹配而被丢弃。
- Subject 不承担请求队列、event bus、网络回调或 Audio callback。
- Desktop 与设备端使用相同 state、generation 和失败语义。
- 配置 Log PAL 时，请求失败日志包含 identity、stage、result、detail 与 bounded frame/byte 统计，且不改变 callback lifecycle。
