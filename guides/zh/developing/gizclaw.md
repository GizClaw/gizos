# GizClaw

`libs/gizclaw` 将 GizClaw C SDK 集成为跨平台 client，提供连接、RegistrationToken 注册、轮询、generic RPC、Server 反向 RPC provider、ping 和 speed test 能力，并提供可由多个产品复用的单 client request service。

## API Reference

[API Reference](/references/gizclaw)

`libs/gizclaw/include` 中实际参与项目构建的头文件是 GizClaw 的生产 Public API contract。Config 提供 server endpoint、private key、cipher mode、timeout 和可选 RPC provider，并注入 PAL mem、HTTP、WebRTC、crypto、time 和 log API。

## 依赖和边界

GizClaw library 负责 SDK 集成和 client protocol，不创建具体 HTTP、WebRTC 或 crypto backend。Credential 来源、连接策略和 app workflow 由调用方负责。

Runtime Profile 负责选择 Workflow driver，`libs/gizclaw` 不在 public Workflow projection 中复制 driver enum，也不要求调用方根据 driver 构造 Workspace 参数。调用方只选择 Workspace 的 PTT 或 Realtime input mode；client 先读取现有 typed `WorkspaceParameters`，仅在参数对象恰好包含 agent type 和 input 时按同一类型更新 input，遇到未知类型或额外、缺失、重复字段时在 PUT 前拒绝，不能覆盖服务端管理的参数。

## Request service

`h2_gizclaw_service_t` 使用调用方注入的 PAL Task、Queue、Sync 和 client config 创建一个 client-owning worker。`submit` 进行 bounded admission 并返回 opaque operation handle；worker FIFO receive typed run callback，组合 caller cancel、service stop 和 operation cancel。普通 API operation 执行完成后只把 operation 放入 completion queue；需要多步交互的 conversation operation 可以在 client I/O 步骤之间调用 `h2_gizclaw_operation_dispatch_call()`，同步请求 dispatch caller 完成一次有界的产品状态步骤，再由同一个 worker 继续发送调用方已经编码的 Opus、poll reply 或关闭 conversation。App main loop 调用有界、非阻塞的 `dispatch`，progress 和 completion callback 才在 dispatch caller thread 执行。Service 可选持有一个 Runtime，只用 `h2_runtime_notify()` 叫醒 main loop 来 dispatch，不产生 Runtime event，不读取 Audio PAL，也不读取或修改产品 state、LVGL subject 或 widget。Progress 和 completion callback 必须保持有界；需要录音、编解码、文件 I/O 或其它长时间工作的调用方必须从 callback 投递到自己拥有的 Task，并立即返回。

Capacity 覆盖 request-queued、running、progress-pending、completion-pending 和 callback-dispatching 的全部 admitted operation，并在 admission 时预留 completion capacity。Progress callback 的返回值同步交回 worker；等待期间的 cancel 或 stop 不再执行该 progress callback，而是唤醒 worker 并把已有 queue entry 转换为唯一 terminal completion。Cancel 是 task-safe、non-blocking 和幂等的；每个 accepted operation 最终恰好产生一次 `FINISHED`、`CANCELED` 或 `SERVICE_CLOSED` completion。Caller-owned typed context 在 callback 返回前保持有效，returned operation handle 由 caller 恰好 release 一次。

正常 domain error 只结束当前 operation。Initial connect、fatal poll 或 transport closed 会关闭 service generation；受影响 operation 以 `SERVICE_CLOSED` 完成，terminal callback 在 operation callback 之后由 `dispatch` 恰好调用一次。Client close 前，optional worker cleanup 先释放仍由 worker 独占的 conversation 等 caller-owned client resource；产品 Audio 和状态仍由 dispatch callback 清理。Teardown 顺序是拒绝新 submit、stop 并 join worker、dispatch drain、release caller handle、deinit；`stop` 不内联执行产品 callback。

Encrypted mode 通过显式 X25519 key/public/shared types、HKDF-SHA256 和对应
AEAD enum 调用 Crypto PAL。GizClaw 的 plaintext mode 在 library 内做经过长度和
capacity 校验的 bounded copy，不把 plaintext 注册成 Crypto PAL algorithm。

## Connection transport 生命周期

`h2_gizclaw_client_connect()` 在返回成功前必须注册 Opus 上下行 media，并建立 connection-scoped Direct Packet 和 Peer Event channel。`libs/gizclaw` 在 connect 前注册 PAL WebRTC media extension；调用方不能把 media 当作可选能力，也不能在连接已建立后替换 extension。RPC 和 HTTP service channel 按调用动态创建，不属于这组固定 transport。

Peer Event 的物理 service channel 由 SDK connection 持有，唯一 access handle 由 `h2_gizclaw_client` 从 connect 成功一直保留到连接关闭。Conversation 只取得该 handle 的逻辑 lease；同一 client 同时只能有一个 conversation。每次 lease 使用 connection 内单调递增且唯一的 input stream ID；服务端可以为下行 `transcript` 和 `assistant` 各自产生 response-local stream ID。Conversation 按 label 分别绑定本轮第一个 response ID，接受其 `:<suffix>` 子流，并丢弃之后不匹配的旧轮文本或 EOS；不能要求下行 ID 等于 input ID。Input 仍然打开时（realtime，server-side VAD），服务端可以打断正在播放的 reply（barge-in）：新 reply 的 BOS 在旧 assistant route 结束前到达时直接取代旧 route，旧 reply 以 `REPLY_DONE` 结束并丢弃已排队的下行 PCM，之后携带 `STREAM_INTERRUPTED` 的旧 EOS 被丢弃；未被取代时该 EOS 本身就是同样的 reply boundary。Input 已经 commit（push-to-talk）后本 generation 不会再有 reply，`STREAM_INTERRUPTED` 保持 `ERROR` 语义。Conversation deinit 只释放逻辑 lease，不释放 client access handle，也不关闭物理 channel；所有 conversation handle 必须先于 client deinit 释放。Direct Packet、Peer Event 或 Opus transport 意外关闭时，`h2_gizclaw_client_poll()` 返回 `H2_PAL_ERR_CLOSED`，调用方必须 close、deinit 并重建完整 client，不能只重开单条 transport。

PAL WebRTC 的 `CLOSED` 和 `ERROR` callback 只提供 callback 期间有效的 borrowed DataChannel handle，backend 可以在 callback 返回后释放它。GizClaw C SDK 必须在 callback 返回前清空 matching service、active RPC、Direct Packet 和 inbound alias；Peer Event 继续保留 SDK-owned service state 供普通 client cleanup 使用，但不再保留 DataChannel alias。后续 request completion、cancellation、client close 或 deinit 只能释放 SDK state，不能再次把已消费的 handle 传给 PAL `channel_close`。显式 close 先于终态 callback 时仍只向 PAL 发起一次 close。

## RPC provider

GizClaw C SDK 的 WebRTC/RPC transport 允许 Server 为 `client.*` method 反向创建 request-scoped Peer RPC channel。SDK 负责接收 request、按 method dispatch、发送 response/error，以及关闭 channel；`libs/gizclaw` 把该入口适配为 GizOS 的 `h2_gizclaw_rpc_provider_fn`，产品 integration 负责提供设备信息、稳定 identifiers 和本地 Tool 实现。

Provider 在 `h2_gizclaw_client_poll()` 所在线程同步运行。上游 C SDK 要求 provider 在返回成功前恰好提交一次 response；GizOS adapter 将这个 responder 细节封装为同步 `out_response`，并在 provider 返回后立即把结果交回上游 responder。Request payload、response payload 和 error message 都是 protobuf byte view：输入只在 callback 期间有效，输出只需保持到 callback 返回，SDK 与 adapter 都不能在返回后继续持有这些 borrowed buffer。

设备主动调用 Server 的 unary 或 server-streaming RPC 与 Server 反向调用 Client provider 是两个方向的 contract。前者由 generic RPC call API 发起；后者只能从 poll 驱动的 provider 入口处理，不能由 UI callback 直接执行，也不能跨线程保留 borrowed payload。产品侧的 state、effect command 和 main-loop 投影规则见 [GizClaw 状态与请求](/apps/gizclaw/state)。

## 上游 API 同步

`@h2_gizclaw_c_sdk//:gizclaw_core` 中的 RPC registry 与 protobuf payload 是 wire contract 的生成结果。Pet、Points 或其它 RPC schema 更新时，先把 `MODULE.bazel` 中 `h2_gizclaw_c_sdk` 的 Release archive URL、SRI integrity 与 `strip_prefix` 原子更新到同一个规范版本，再同步已有 `libs/gizclaw` stable wrapper；不能只修改手写 method number、复制旧 protobuf struct，或只更新产品文档。没有 GizOS-owned domain/lifecycle 语义的 RPC（例如 Firmware metadata）直接使用 generic RPC API 与 pinned generated schema，不为相同字段再增加一层 typed wrapper。GizOS 中公开的 RPC method 常量通过 compile-time assertion 与上游 registry 对齐，registry 再次漂移时必须使 build 失败。

Archive 自带 Bazel targets、生成代码和精确的 nanopb runtime；GizOS 通过 `use_repo_rule(http_archive)` 声明可传递给下游 Bzlmod consumer 的 immutable repository，不再注入 BUILD overlay、单独解析 nanopb 或维护 SDK source patch。该 archive 尚未发布到 Bazel Central Registry，因此不能使用只在根 module 生效的 `archive_override` 作为传递依赖。具体版本和完整性校验以 `MODULE.bazel` 中的 `h2_gizclaw_c_sdk` 声明为准。

Wire message 使用 `name` / `*_name`。GizOS wrapper 将 Peer-addressable resource 继续公开为 name；将 occurrence、relationship、history 和 ledger 的 wire name 逐字节映射到既有 public `id` / `*_id`，不做 trim、派生、翻译或 storage-ID 替换。技术性 transport request ID、idempotency key 和 `gear_id` 不属于这层业务 identity 映射：

| 资源 | Peer selector / projection |
| --- | --- |
| Registration / Firmware / Speech | `runtime_profile_name`；Firmware 由 channel 选择；Speech 使用 `*_model_name` |
| Workflow / Workspace | Workflow `name`、Workspace `name` / `workflow_name`；history public `id` / `history_id` 映射 wire `name` / `history_name` |
| Pet / gameplay / Points | resource `name`、`pet_def_name`、`pet_name`、`game_name`；GameResult、reward、transaction、source public `*_id` 映射对应 wire `*_name` |
| Contact | immutable caller-local `name`、mutable `display_name` |
| Friend / FriendGroup | Friend/member/history public ID 映射 wire name；FriendGroup `name` / `friend_group_name` 与独立 display name 保持 name 语义 |

Runtime Profile alias（包括 Workflow `name` 与 Workspace `workflow_name`）最长
63 字节，由 `.` 分隔的 lowercase kebab-case segment 组成；完整 alias 是不拆分、
不归一化的 opaque key。`libs/gizclaw` 在 catalog decode、Workflow get 和
Workspace create 边界使用该 grammar。Collection、Workspace `name`、history
public ID 和其它非 Runtime Profile identifier 继续使用各自 contract，不能因为
alias 支持 `.` 而一并放宽。

Firmware channel 是原子 breaking update，不保留旧 `firmware_name` alias 或探测
服务端版本；上述 record/relationship public ID 则是兼容 contract。method 64 不再是
FriendGroup message send；该 RPC 和 wrapper 已删除，后续方法按生成 registry 重编号，
FriendGroup message audio-get 固定为 method 95。生成的 protobuf/RPC 文件只能随
upstream archive pin 更新，GizOS 不手工修改。

API Reference 从 `libs/gizclaw/include` 的生产 Public Header 生成。完成上游同步和 adapter 修改后，在仓库根目录运行 `make guides-build`，使 `/references/gizclaw` 展示当前 wrapper 的 method、参数、ownership 和生命周期；不手工编辑 `.generated/api`。

`RegistrationToken` 是稳定、预分发的产品到 RuntimeProfile binding，不是一次
注册后即失效的 credential。每条新的 Peer connection 都通过
`h2_gizclaw_client_register()` 提交同一个产品 token，以取得该 connection 的 profile
snapshot；client 不消费或缓存 token。单测必须覆盖同一 token 的重复成功调用和返回 binding。

## 构建与测试

```sh
bazel test //libs/gizclaw:all
```

外部 E2E 验收位于 `projects/e2e/targets/cc_test/gizclaw`，使用三个新注册的
`h106-tiga` Peer，经系统 DNS 连接 E2E 自然入口 `e2e.gizclaw.com:9821`，或在国内
显式选择北京入口 `edge-bj-01.e2e.gizclaw.com:9821`。测试通过
production `h2_desktop_platform_webrtc_api()` 选择的 backend 执行 public RPC、
双向语音和 history audio；RPC coverage manifest 必须覆盖全部公开 method enum 和
server-facing wrapper。每个 `live` 或 `cleanup` method 显式映射到一个 E2E evidence
symbol；suite 只有在该 symbol 于同一次进程运行中报告 PASS 后才接受对应 method。
它是显式手动测试，不进入普通 native package CI。

Firmware 与 Voice 还由独立的 Pion `manual` test 将完全相同的 public flow 接到
`libs/pal/providers/pion`。该 Go/Pion static archive 只实现
`h2_pal_webrtc_api_t`：Pion goroutine 复制事件，PAL callback 只由调用方的
`peer_poll()` 线程分发，C ABI 只交换整数 handle 和同步 borrowed buffer。它是归因工具，
不是 `libs/gizclaw` dependency，也不替换 Desktop production H2Peer accessor。

live suite 还必须验证 pinned GizClaw C SDK 的 single-client 并发能力。当前
`v0.3.1` 提供 request-owned unary handle；concurrency suite 必须在一个 active client
上依次启动三个 Ping handle，由唯一 serialized poll owner 推进，记录三个不同 stream ID
的 request DataChannel、三个 terminal result、零残留 channel 和恢复
Ping。不得用三个线程调用共享 client，也不得用三个 client/Peer 或三个串行请求伪造支持。
社交 fixture 的 helper Peer 只用于 Friend/FriendGroup 建模。除此之外，测试还必须验证两个 Peer 可各自使用相同 Workspace、Contact、FriendGroup 和
Pet name 且互不可见，同一 Peer reconnect 后可恢复原 Workspace/history，并覆盖 method
95 的 metadata、stream byte count 与清理失败路径。Linux x86_64 与 macOS arm64 都是
merge 前的外部服务合同证据。

测试创建的 Workspace 和 Pet 分别通过
`h2_gizclaw_client_workspace_delete()` 与 `h2_gizclaw_client_pet_delete()` 删除；成功
返回的 snapshot 按普通 owned-output 规则用对应 deinit API 释放。所有业务资源清理
完成后才请求 Peer 删除。
