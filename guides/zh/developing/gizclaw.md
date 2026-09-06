# GizClaw

`libs/gizclaw` 将 GizClaw C SDK 集成为跨平台 client，提供连接、RegistrationToken 注册、轮询、generic RPC、Server 反向 RPC provider、ping 和 speed test 能力，并提供可由多个产品复用的单 client request service。

## API Reference

[API Reference](/references/gizclaw)

`libs/gizclaw/include` 中实际参与项目构建的头文件是 GizClaw 的生产 Public API contract。Config 提供 server endpoint、private key、cipher mode、timeout 和可选 RPC provider，并注入 PAL mem、HTTP、WebRTC、crypto、time 和 log API。

## 依赖和边界

GizClaw library 负责 SDK 集成和 client protocol，不创建具体 HTTP、WebRTC 或 crypto backend。Credential 来源、连接策略和产品选择由调用方负责。Session 统一持有 Runtime Profile、Workflow catalog、Workspace 与 Conversation 的公共状态，准备、版本一致性和失败处理见[状态与请求](/apps/gizclaw/state)。产品不再重复实现这些准备流程。

Runtime Profile 负责选择 Workflow driver，`libs/gizclaw` 不在 public Workflow projection 中复制 driver enum，也不要求调用方根据 driver 构造 Workspace 参数。Workspace 更新统一使用 `h2_gizclaw_*workspace_set_parameters`，对应 SDK 0.15.5 的 `server.workspace.parameters.set`（110）；旧的 `workspace_set_input` 入口已删除。

依赖的 GizClaw C SDK 已升级到 0.15.6。`h2_gizclaw_*workspace_activate` 保留 SET-only 语义，`h2_gizclaw_*workspace_reload` 保留重载当前选择的行为。新增 `h2_gizclaw_*workspace_reload_with_options`（RPC 120），在一次调用中可选地选择 Workspace、应用参数补丁，然后重载。传入零长度 `name` 保持当前选择，`parameters == NULL` 不修改参数；非空补丁复用 `h2_gizclaw_workspace_parameters_patch_t` 的字段 presence 语义。请求创建时复制参数，响应仍为 `h2_gizclaw_workspace_activation_t`。

`h2_gizclaw_workspace_parameters_patch_t` 通过独立 `has_*` 标记选择 input（PTT/Realtime）、conversation initiative（peer/agent）和 agent initiative policy（once_when_empty/on_reload）。`workspace_set_parameters` 至少指定一个字段；显式的无效枚举值、空 patch、非法名称在发送前返回 INVALID_ARG。create 编码并持有 patch 数据，调用方随后可释放或修改原对象；同步入口沿用同一 request/parse 流程。

客户端只发送指定字段，不先 GET typed `WorkspaceParameters`，不解析或重写其 agent_type，也不再依据未知、额外、缺失或重复的服务端 typed 参数字段拒绝更新。服务端根据绑定的 Workflow driver 校验 patch、合并指定字段并保留其他参数；不支持的 driver/字段通过原有远端错误路径返回。公开 patch 是固定的可写字段集合，不是对服务端 metadata 的封闭枚举。SFU input 支持由上游实现，E2E 保留真实配置请求，不能通过跳过它声称完整验收通过。

## Request service

`h2_gizclaw_service_t` 使用调用方注入的 PAL Task、Queue、Sync 和 client config 创建一个 client-owning worker。`submit` 进行 bounded admission 并返回 opaque operation handle；worker FIFO receive typed run callback，组合 caller cancel、service stop 和 operation cancel。普通 API operation 执行完成后只把 operation 放入 completion queue；需要多步交互的 conversation operation 可以在 client I/O 步骤之间调用 `h2_gizclaw_operation_dispatch_call()`，同步请求 dispatch caller 完成一次有界的产品状态步骤，再由同一个 worker 继续发送调用方已经编码的 Opus、poll reply 或关闭 conversation。App main loop 调用有界、非阻塞的 `dispatch`，progress 和 completion callback 才在 dispatch caller thread 执行。Service 可选持有一个 Runtime，只用 `h2_runtime_notify()` 叫醒 main loop 来 dispatch，不产生 Runtime event，不读取 Audio PAL，也不读取或修改产品 state、LVGL subject 或 widget。Progress 和 completion callback 必须保持有界；需要录音、编解码、文件 I/O 或其它长时间工作的调用方必须从 callback 投递到自己拥有的 Task，并立即返回。

Capacity 覆盖 request-queued、running、progress-pending、completion-pending 和 callback-dispatching 的全部 admitted operation，并在 admission 时预留 completion capacity。Progress callback 的返回值同步交回 worker；等待期间的 cancel 或 stop 不再执行该 progress callback，而是唤醒 worker 并把已有 queue entry 转换为唯一 terminal completion。Cancel 是 task-safe、non-blocking 和幂等的；每个 accepted operation 最终恰好产生一次 `FINISHED`、`CANCELED` 或 `SERVICE_CLOSED` completion。Caller-owned typed context 在 callback 返回前保持有效，returned operation handle 由 caller 恰好 release 一次。

正常 domain error 只结束当前 operation。Initial connect、fatal poll 或 transport closed 会关闭 service generation；受影响 operation 以 `SERVICE_CLOSED` 完成，terminal callback 在 operation callback 之后由 `dispatch` 恰好调用一次。Client close 前，optional worker cleanup 先释放仍由 worker 独占的 conversation 等 caller-owned client resource；产品 Audio 和状态仍由 dispatch callback 清理。Teardown 顺序是拒绝新 submit、stop 并 join worker、dispatch drain、release caller handle、deinit；`stop` 不内联执行产品 callback。

Encrypted mode 通过显式 X25519 key/public/shared types、HKDF-SHA256 和对应
AEAD enum 调用 Crypto PAL。GizClaw 的 plaintext mode 在 library 内做经过长度和
capacity 校验的 bounded copy，不把 plaintext 注册成 Crypto PAL algorithm。

## Connection transport 生命周期

`h2_gizclaw_client_connect()` 在返回成功前必须注册 Opus 上下行 media，并建立 connection-scoped Direct Packet 和 Peer Event channel。`libs/gizclaw` 在 connect 前注册 PAL WebRTC media extension；调用方不能把 media 当作可选能力，也不能在连接已建立后替换 extension。RPC 和 HTTP service channel 按调用动态创建，不属于这组固定 transport。

Peer Event 的物理 service channel 由 SDK connection 持有，唯一 access handle 由 `h2_gizclaw_client` 从 connect 成功一直保留到连接关闭。Conversation 只取得该 handle 的逻辑 lease；同一 client 同时只能有一个 conversation。每次 lease 使用 connection 内单调递增且唯一的 input stream ID；服务端可以为下行 `transcript` 和 `assistant` 各自产生 response-local stream ID。Conversation 按 label 分别绑定本轮第一个 response ID，接受其 `:<suffix>` 子流，并丢弃之后不匹配的旧轮文本或 EOS；不能要求下行 ID 等于 input ID。Input 仍然打开时（realtime，server-side VAD），服务端可以打断正在播放的 reply（barge-in）：新 reply 的 BOS 在旧 assistant route 结束前到达时直接取代旧 route，旧 reply 以 `REPLY_DONE` 结束并丢弃已排队的下行 PCM，之后携带 `STREAM_INTERRUPTED` 的旧 EOS 被丢弃；未被取代时该 EOS 本身就是同样的 reply boundary。Input 已经 commit（push-to-talk）后本 generation 不会再有 reply，`STREAM_INTERRUPTED` 保持 `ERROR` 语义。每个 reply 投递给 App 的 conversation event 数量有界：下行 PCM 只写入绑定的 Track，不经 event 复制，第一块 PCM 进入 Track 后、该 reply 的 boundary 之前投递一次 `REPLY_AUDIO_STARTED`，文本事件与它没有顺序关系（文本流独立，可能先到），最后恰好一次 `REPLY_DONE` 或 `ERROR`；dispatch wake 不随 reply 长度增长。Conversation deinit 只释放逻辑 lease，不释放 client access handle，也不关闭物理 channel；所有 conversation handle 必须先于 client deinit 释放。Direct Packet、Peer Event 或 Opus transport 意外关闭时，`h2_gizclaw_client_poll()` 返回 `H2_PAL_ERR_CLOSED`，调用方必须 close、deinit 并重建完整 client，不能只重开单条 transport。

PAL WebRTC 的 `CLOSED` 和 `ERROR` callback 只提供 callback 期间有效的 borrowed DataChannel handle，backend 可以在 callback 返回后释放它。GizClaw C SDK 必须在 callback 返回前清空 matching service、active RPC、Direct Packet 和 inbound alias；Peer Event 继续保留 SDK-owned service state 供普通 client cleanup 使用，但不再保留 DataChannel alias。后续 request completion、cancellation、client close 或 deinit 只能释放 SDK state，不能再次把已消费的 handle 传给 PAL `channel_close`。显式 close 先于终态 callback 时仍只向 PAL 发起一次 close。

## RPC provider

GizClaw C SDK 的 WebRTC/RPC transport 允许 Server 为 `client.*` method 反向创建 request-scoped Peer RPC channel。SDK 负责接收 request、按 method dispatch、发送 response/error，以及关闭 channel；`libs/gizclaw` 把该入口适配为 GizOS 的 `h2_gizclaw_rpc_provider_fn`，产品 integration 负责提供设备信息、稳定 identifiers、本地 Tool 以及设备控制实现。

SDK 0.15.5 的标准设备控制由 Service 内置 provider 实现。在现有
`h2_gizclaw_config_t` 中分别传入 `audio`、`wifi`、`wifi_settings`、`power`；
HTTP、Time、Crypto、allocator 复用已有字段，Task、Queue、Sync 复用 Service 配置。
不需要第二份 device config、device 实例或额外 start/stop 调用。

厂商、型号、硬件版本和序列号通过 `manufacturer`、`model`、
`hardware_revision`、`serial` 提供。标准 RPC 的 protobuf 编解码、校验、响应由库处理；
`rpc_provider` 保留为产品自定义方法的 fallback。没有配置的标准能力返回
`UNIMPLEMENTED`，不会返回虚假的成功 ACK。

- Service 配置 Runtime 且 Audio 来自该 Runtime 时，音量与静音直接读写 Runtime audio state，GizClaw 不保留私有缓存。静音保留设定音量，本地 percent 调整取消静音并更新同一 state。只注入原始 PAL、没有 Runtime 的 library caller 仅能报告有效音量，静音时为零；需要逻辑静音恢复的产品必须提供 Runtime。
- Wi-Fi 状态/扫描/连接使用 PAL Wi-Fi，保存网络使用 PAL Wi-Fi Settings。产品注入 `runtime->wifi_sta` 与 `runtime->wifi_settings` 后，连接由 Runtime 等待 GOT_IP 并持久化凭据；GizClaw 不维护另一份网络记录。RPC list 如实返回现有 PAL Settings 的 0 或 1 条。直接注入原始 PAL 的调用方仍自行承担持久化策略。RPC response 是动作接受结果，后续连接或保存失败通过设备日志记录，不把接受 ACK 当作连接成功。
- 普通重启直接使用 PAL Power。重启、切网、OTA 在本地 RPC response 发送完成后
  才交给 `$gizclaw/device` task；回复发送失败或 Service 停止会取消待执行动作。
- 传入 `audio` PAL 即启用 Ogg/Opus 播放器。`audio_buffer_bytes` 设置压缩数据环形
  缓冲容量（默认 64 KiB），`audio_prebuffer_bytes` 设置起播和缺数据后的预缓冲量
  （默认 min(16 KiB, 缓冲容量)）。HTTP task 和播放 task 并行，缓冲满时通过背压暂停
  读取，边下载边解析 Ogg page、解码 Opus，不限制整首音频长度。短音频在下载结束后
  使用已有数据起播；持续缺数据超时会取消下载并上报错误。
  解码器保留一个最大 65,307 字节 Ogg page，跨页 packet 上限 64 KiB，独立于环形缓冲。
  以 16 kHz mono PCM16LE 写入 PAL Audio track，按 PAL 报告的帧大小拼帧，末帧补零
  不计入播放进度。不支持 Vorbis、AAC 或 MP3。库只关闭自己的 track，不关闭共享 speaker。
- 播放列表支持最多 32 项、读取/替换/追加、从指定索引播放、停止和 off/one/all
  循环模式。失败的列表校验保留旧列表和播放；停止或替换取消在途下载/播放。
  播放中进度按已写入 PCM 扣除队列容量及一个在途帧保守估算，结束时 drain 后
  校准到全部源采样；不逐帧 drain，避免插入静音。状态变化及约每秒进度通过 telemetry
  异步提交，不阻塞播放等待网络上报。
- `h2_gizclaw_vtable_t` 只补 PAL 缺少的产品事实、命名提示音到 HTTPS Ogg/Opus URL
  的解析，以及 H2Loader Stage begin/write/finish/abort/activate。`get_facts` 在
  RPC owner 上运行，必须快速返回；提示音解析和 Stage 操作在设备 task 上运行。
  回调不得直接销毁或停止 Service；activate 应向产品 owner 投递升级动作。
- OTA 使用明确的 `firmware_channel`，允许 RPC 覆盖 channel 并附带期望 SHA-256。
  库获取元数据并通过 PAL HTTP 下载；Stage backend 必须验证 package 的长度、
  SHA-256、board/target 和 manifest，验证通过才能发布 Stage。库上报 started、
  downloading、failed；安装后新固件核对运行身份，使用保存的 update_id 上报 succeeded。

C SDK 的 provider 合同仍是同步回复，所以 Wi-Fi scan 在 RPC owner 上执行有界 PAL
扫描（默认 5 秒、最多 30 秒）。下载、音频解码/播放和 OTA 均在独立设备 task 上执行。
长扫描期间会占用 RPC owner；不能用一个提前 ACK 冒充扫描结果。

应用主动上报时，`h2_gizclaw_telemetry_observation_t` 增加 `AUDIOPLAYER` 和 `OTA`。
OTA frame 必须只包含一条 OTA observation，以映射 SDK 独立的 OTA frame API；
其余 observation 继续使用原有批量 frame。上报成功仅表示本地 transport 接受。

设备身份可用 `h2_gizclaw_rpc_api_key_create()` 创建 HTTP API key，用
`h2_gizclaw_rpc_api_key_revoke()` 撤销；也提供相应 create/do/wait/parse/release 接口。
返回的 secret 由调用者管理，不应写入日志。

Provider 在 `h2_gizclaw_client_poll()` 所在线程同步运行。上游 C SDK 要求 provider 在返回成功前恰好提交一次 response；GizOS adapter 将这个 responder 细节封装为同步 `out_response`，并在 provider 返回后立即把结果交回上游 responder。Request payload、response payload 和 error message 都是 protobuf byte view：输入只在 callback 期间有效，输出必须在 callback 返回后保持有效，直到 adapter 消费返回的响应；不能返回栈上 buffer。

设备主动调用 Server 的 unary 或 server-streaming RPC 与 Server 反向调用 Client provider 是两个方向的 contract。前者由 generic RPC call API 发起；后者只能从 poll 驱动的 provider 入口处理，不能由 UI callback 直接执行，也不能跨线程保留 borrowed payload。产品侧的 state、effect command 和 main-loop 投影规则见 [GizClaw 状态与请求](/apps/gizclaw/state)。

## 设备 Debug 访问模式

`h2_gizclaw_req_create_debug_set()` 使用当前 SDK 0.15.5 已有的
`server.runtime.put` 和 `ServerPutRuntimeRequest.debug_mode`。设备以当前
Service 的自身身份设置 `off`、`readonly` 或 `fullcontrol`，服务端负责持久化
和 SN／IMEI 查询后的访问控制；这不是本地日志等级，也不需要向工程师提供设备
private key。

创建请求时复制 mode，不产生网络请求。调用方使用标准
`req_do`、`req_wait`／`req_cancel`、`resp_parse_debug_set`、`req_release`
生命周期；UI 不应阻塞等待网络。只有成功解析服务器响应后才更新显示状态，
失败不能显示为已开启。响应保留未知 mode 文本，不能把未知值解释为
`fullcontrol`。关闭使用同一个接口发送 `off`。

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


### 设备控制回复后的本地动作

产品 provider 可以在成功响应中设置 `on_complete` 与 `complete_user`。GizOS 将其关联到当前入站 RPC 通道，在 SDK 接受响应写入并正常关闭该通道后，从 poll owner 调用一次。普通 poll 返回成功不表示该通道已经完成；非终态的 WOULD_BLOCK、TIMEOUT 或其他 poll error 也不能取消尚未关闭的通道。成功必须同时具备该通道响应 EOS 已被 PAL 接受、SDK 本地关闭的证据；adapter 按通道跟踪已接受的帧，支持跨 send 分片与背压重试。同一次 poll 中其他通道超时或失败，不会撤销已成功关闭的响应。未发完 EOS 的通道即使本地关闭也只能报告失败。发送阻塞时继续保留动作，编码错误、发送失败、远端取消和客户端停止则以非 OK 结果撤销。关闭或销毁客户端时，在对应 owner 上释放未完成动作；注册失败可能在 provider 内同步返回失败通知。每个 client 最多保留四个完成回调；满容量、缺少当前入站通道或错误响应附带回调时，新回调同步收到 INVALID_STATE，响应不提交，已有槽位不受影响。

这是本地发送生命周期，不是对端收到或处理回复的确认，不新增网络消息，也不更改 GizClaw 0.15.3 SDK。回调只能向产品 owner 发布待执行动作，不能阻塞或重入 client API。产品参数必须复制到自身状态，生命周期覆盖完成或取消；不能保留借用请求、栈上的响应或已释放的 user。H106 的重启和临时切网消费此路径，OTA 是否支持仍由产品决定。
