# GizClaw

`libs/gizclaw` 将 GizClaw C SDK 集成为跨平台 client，提供连接、RegistrationToken 注册、轮询、generic RPC、Server 反向 RPC provider、ping 和 speed test 能力，并提供可由多个产品复用的单 client request service。

`h2_gizclaw_config_t.allocator` 继续传给 HTTP 请求，并贯穿 WebRTC peer 和设备播放器音轨。WebRTC provider 未实现 allocator 扩展并返回 `UNSUPPORTED` 时，client 记录 `peer_allocator_unsupported`，再用 provider 默认分配器创建 peer。产品可以让 GizClaw 与 Lua Host 共用一个 arena，但必须等子任务 join、音轨关闭和 owned WebRTC event 全部释放后再销毁 arena。Service、时间同步、设备 worker 和音频下载任务的栈由平台 task provider 配置；ESP internal 栈策略与 provider 的独立 internal/control/packet 存储不受 client allocator 覆盖。

## API Reference

[API Reference](/references/gizclaw)

`libs/gizclaw/include` 中实际参与项目构建的头文件是 GizClaw 的生产 Public API contract。Config 提供 server endpoint、private key、cipher mode、timeout 和静态 Tool handler 与 MHS state 表，并注入 PAL mem、HTTP、WebRTC、crypto、time 和 log API。

## 依赖和边界

GizClaw library 负责 SDK 集成和 client protocol，不创建具体 HTTP、WebRTC 或 crypto backend。Credential 来源、连接策略和 app workflow 由调用方负责。

Runtime Profile 负责选择 Workflow driver，`libs/gizclaw` 不在 public Workflow projection 中复制 driver enum，也不要求调用方根据 driver 构造 Workspace 参数。Workspace 更新统一使用 `h2_gizclaw_*workspace_set_parameters`，对应 SDK 0.15.5 的 `server.workspace.parameters.set`（110）；旧的 `workspace_set_input` 入口已删除。

依赖的 GizClaw C SDK 固定为 0.23.1。自 0.17.0 起，`h2_gizclaw_*workspace_activate` 保留 SET-only 语义，`h2_gizclaw_*workspace_reload` 保留重载当前选择的行为。新增 `h2_gizclaw_*workspace_reload_with_options`（RPC 120），在一次调用中可选地选择 Workspace、应用参数补丁，然后重载。传入零长度 `name` 保持当前选择，`parameters == NULL` 不修改参数；非空补丁复用 `h2_gizclaw_workspace_parameters_patch_t` 的字段 presence 语义。请求创建时复制参数，响应仍为 `h2_gizclaw_workspace_activation_t`。

`h2_gizclaw_workspace_parameters_patch_t` 通过独立 `has_*` 标记选择 input（PTT/Realtime）、conversation initiative（peer/agent）、agent initiative policy（once_when_empty/on_reload）、TTS 语速和安全围栏档位（见 [Workspace 参数补丁](#workspace-参数补丁)）。`workspace_set_parameters` 至少指定一个字段；显式的无效枚举值、空 patch、非法名称在发送前返回 INVALID_ARG。create 编码并持有 patch 数据，调用方随后可释放或修改原对象；同步入口沿用同一 request/parse 流程。

客户端只发送指定字段，不先 GET typed `WorkspaceParameters`，不解析或重写其 agent_type，也不再依据未知、额外、缺失或重复的服务端 typed 参数字段拒绝更新。服务端根据绑定的 Workflow driver 校验 patch、合并指定字段并保留其他参数；不支持的 driver/字段通过原有远端错误路径返回。公开 patch 是固定的可写字段集合，不是对服务端 metadata 的封闭枚举。SFU input 支持由上游实现，E2E 保留真实配置请求，不能通过跳过它声称完整验收通过。

0.17.0 已移除 Gameplay：GizOS 不再公开 Pet、积分、领养和宠物 PIXA 下载 API、RPC 常量或对应 Resource。E2E 不再创建这些资源或要求这些调用的覆盖证据。Workspace 图标 PIXA 下载、音频播放器和本地游戏继续由各自接口负责。

0.18.0 相对 0.17.0 只有增量：新增 RPC 123–127、对应 protobuf message，`WorkspaceHistoryListRequest` 增加可选 `start_time_ms` / `end_time_ms`（GizOS 暂不暴露，仍用生成的 `_init_zero` 初始化），以及 GizOS 不使用的 control API（device find、device runtime-profile）。GizOS 侧无需适配已有调用。

0.18.5 相对 0.18.0 只有增量，RPC registry 与 core 源码不变：`FriendObject` 增加仅由 `server.friend.list` 填写的 `online`、`last_seen_at`、`display_name`、`emoji`；`DoubaoRealtimeWorkflowSpec` 增加可选 `tts`（GizOS 不解析）；control API 新增的方法 GizOS 不使用。`h2_gizclaw_friend_t` 因此在好友列表中直接带出 `name` / `emoji`（未设置为 NULL，显式设为空时为 `""`，与 `friend_info_get` 相同）以及 `has_online` / `online` / `last_seen_at`；`friend_info_get` 仍只投影资料，不带在线状态（`has_online` 为 false）。

0.18.7 相对 0.18.5 只有增量：`FriendGroupMemberObject` 增加仅由 `server.friend_group.members.list` 填写的 `online` / `last_seen_at`；control API 的变化 GizOS 不使用。`h2_gizclaw_friend_group_member_t` 因此在成员列表中带出 `has_online` / `online` / `last_seen_at`：`online` 表示成员设备是否连接到回答请求的 Server，未报告在线状态（包括 presence 读取失败或旧服务端未提供字段）时 `has_online` 为 false；`last_seen_at` 是 UTC RFC 3339 文本，最多 64 字节，Server 从未观察到该成员时为 NULL。超长、含 NUL 或非法 UTF-8 的时间文本使整页返回 `H2_PAL_ERR_FORMAT` 并回滚 storage。member add/put/delete 仍不带 presence（`has_online` 为 false，`last_seen_at` 为 NULL），即使响应携带这些字段也忽略；公开 API 函数数量不变。

设备协议使用 `mhs/v0` 状态读写和 `tool/v0` 预定义操作；GizOS 的公共 enum 在编译期与 SDK registry 校验。Workspace 参数补丁仍提供 input、conversation 与 tts_speech_rate_percent，默认不发送新增的可选 wire 字段。Workflow 返回不可变 name 与普通字符串 tags；Workspace 只通过 workflow_name 关联 Workflow。

设备间社交提醒使用三组 typed wrapper，沿用 Social 的 create/parse/sync 生命周期：

- `h2_gizclaw_*friend_ping`（`server.friend.ping`，123）按 Friend relationship ID（`h2_gizclaw_friend_t.id`）提醒好友设备；`h2_gizclaw_*friend_group_ping`（`server.friend_group.ping`，124）按 FriendGroup name 召集组内其他成员设备。名称校验与 `friend_info_get` / `friend_group_get` 相同，create 复制名称、不发网络请求。结果写入调用方持有的 `h2_gizclaw_social_ping_t`，不需要 response storage：`DELIVERED` 带 `delivered_count`（好友 ping 恰好 1，召集至少 1）；`NOT_ONLINE` 表示目标都不在线，服务端既没有发送也没有开启限流窗口；`RATE_LIMITED` 只在此时带 `retry_after_seconds`（≥1）。未知 result 或违反上述组合的回复按 `H2_PAL_ERR_FORMAT` 失败，不猜测含义；限流是正常结果，不是 RPC 错误。
- `h2_gizclaw_*public_profile_get`（`server.profile.get`，125）按公钥批量查询 1–16 个 Peer 的公开资料，仅含自选 `display_name` 与 `emoji`，Peer 不存在或未设置时对应字段为 NULL。每个 key 须为 1–64 字节可打印 ASCII，规范编码由服务端判定；重复 key 原样发送、服务端只回一次。解析要求 item 恰好是按请求顺序去重后的 key，写入调用方 response storage 并在失败时回滚。该接口与查询自身资料的 `h2_gizclaw_*profile_get`（`server.info.get`）是两个 contract。

FriendGroup 成员管理按 Social 的 create/parse/sync 生命周期覆盖 `server.friend_group.members.*` 全部四个方法：`h2_gizclaw_*friend_group_member_list`（58）、`h2_gizclaw_*friend_group_member_add`（59）、`h2_gizclaw_*friend_group_member_put`（60）和 `h2_gizclaw_*friend_group_member_delete`（61）。`member_add` 由组内 owner 或 admin 按公钥（如 `h2_gizclaw_friend_t.peer_public_key`，1–64 字节可打印 ASCII）直接把一个 Peer 加入调用方的 FriendGroup，无需邀请码；`member_name` 是被加入者在自己列表里看到的组名，校验与 FriendGroup name 相同；角色只能是 ADMIN 或 MEMBER。create 复制全部输入、不发网络请求；parse 与 `member_put` / `member_delete` 相同：把返回的成员解码进调用方 response storage，不与请求交叉比对；回复缺少 value、成员 ID 或组名、角色未知、文本含 NUL 或非法 UTF-8 时按 `H2_PAL_ERR_FORMAT` 失败并回滚。

谁能加、能加多少由 Server 判定：ADMIN 只能由 owner 添加，MEMBER 可由 owner 或 admin 添加；一个 FriendGroup 连同 owner 最多 10 人，一个 Peer 最多加入 10 个 FriendGroup；Server 不要求双方已是 Friend。重复添加同一成员且 `member_name` 不变时按改角色处理，owner 不能被重复添加。错误码沿用所有 Social wrapper 的统一映射：调用方不在该组（组对其不可见）返回 `H2_PAL_ERR_NOT_FOUND`；组已满（`RESOURCE_EXHAUSTED` / `FRIEND_GROUP_FULL`）、对方组数已满（`FRIEND_GROUP_LIMIT_REACHED`）、权限不足、`member_name` 冲突和并发修改都返回 `H2_GIZCLAW_ERR_REMOTE`，库不按单个 RPC 细分这些远端原因。

`device.find` 与 `social.ping` 是由产品注册的 tool/v0 handler，见下文设备 provider。

## Request service

`h2_gizclaw_service_t` 使用调用方注入的 PAL Task、Queue、Sync 和 client config 创建一个 client-owning worker。`submit` 进行 bounded admission 并返回 opaque operation handle；worker FIFO receive typed run callback，组合 caller cancel、service stop 和 operation cancel。普通 API operation 执行完成后只把 operation 放入 completion queue；需要多步交互的 conversation operation 可以在 client I/O 步骤之间调用 `h2_gizclaw_operation_dispatch_call()`，同步请求 dispatch caller 完成一次有界的产品状态步骤，再由同一个 worker 继续发送调用方已经编码的 Opus、poll reply 或关闭 conversation。App main loop 调用有界、非阻塞的 `dispatch`，progress 和 completion callback 才在 dispatch caller thread 执行。Service 可选持有一个 Runtime，只用 `h2_runtime_notify()` 叫醒 main loop 来 dispatch，不产生 Runtime event，不读取 Audio PAL，也不读取或修改产品 state、LVGL subject 或 widget。Progress 和 completion callback 必须保持有界；需要录音、编解码、文件 I/O 或其它长时间工作的调用方必须从 callback 投递到自己拥有的 Task，并立即返回。

Capacity 覆盖 request-queued、running、progress-pending、completion-pending 和 callback-dispatching 的全部 admitted operation，并在 admission 时预留 completion capacity。Progress callback 的返回值同步交回 worker；等待期间的 cancel 或 stop 不再执行该 progress callback，而是唤醒 worker 并把已有 queue entry 转换为唯一 terminal completion。Cancel 是 task-safe、non-blocking 和幂等的；每个 accepted operation 最终恰好产生一次 `FINISHED`、`CANCELED` 或 `SERVICE_CLOSED` completion。Caller-owned typed context 在 callback 返回前保持有效，returned operation handle 由 caller 恰好 release 一次。

正常 domain error 只结束当前 operation。Initial connect、fatal poll 或 transport closed 会关闭 service generation；受影响 operation 以 `SERVICE_CLOSED` 完成，terminal callback 在 operation callback 之后由 `dispatch` 恰好调用一次。Client close 前，optional worker cleanup 先释放仍由 worker 独占的 conversation 等 caller-owned client resource；产品 Audio 和状态仍由 dispatch callback 清理。Teardown 顺序是拒绝新 submit、stop 并 join worker、dispatch drain、release caller handle、deinit；`stop` 不内联执行产品 callback。

`h2_gizclaw_service_deinit()` 在尚未停止或仍有持有者等拒绝条件成立时返回 `H2_PAL_ERR_INVALID_STATE`，并以 WARN `stage=service_deinit_blocked` 列出全部拒绝状态。`stopped`、`dispatch`、`active` 分别表示已停止、正在 dispatch 和 active operation 数；`refs=caller/request/track/downlink` 按固定顺序给出四类引用数；`unset` 表示 Track 正在解绑，`queued` 和 `items` 分别是排队事件数和 dispatch 项数；`terminal=pending/dispatched` 与 `attached=audio_conversation/session` 分别按固定顺序给出两个布尔状态。该紧凑格式在七个计数均为 64 位 `SIZE_MAX` 时仍不超过 `H2_PAL_LOG_MESSAGE_MAX`（含 NUL 共 256 字节），保留完整计数值和末尾字段。状态在 service lock 内格式化，在解锁后记录；拒绝状态的签名与上次相同时不重复记录，签名变化才再记录。拒绝条件、返回值和成功 teardown 流程保持不变。

Encrypted mode 通过显式 X25519 key/public/shared types、HKDF-SHA256 和对应
AEAD enum 调用 Crypto PAL。GizClaw 的 plaintext mode 在 library 内做经过长度和
capacity 校验的 bounded copy，不把 plaintext 注册成 Crypto PAL algorithm。

## Workflow tags 与 Workspace identity

Workflow list 接受最多 32 个 `h2_gizclaw_str_t` tag，每个 1–128 UTF-8 字节且不含 NUL。多个 tag 使用精确 AND 匹配，零 tag 返回全部；重复 selector 保留在请求中并沿用服务端的幂等匹配语义。Library 不 trim、不折叠大小写、不解析年龄或类别，也不把 tags 当作授权条件。请求创建时复制编码后的 bytes；返回的 tags 字符串数组随 response storage 存活，空数组是有效 Workflow。

Workspace list 没有 collection 参数，返回当前 Peer 可访问的 Workspace；create 只需要 workflow_name 和 Workspace name。Workflow 的 tags 改变不会改变其 name，也不会重绑定已有 Workspace。Session 的 tags 只控制 catalog 查询；按 workflow_name 选择时，完整 catalog 的未命中项会通过 Workflow get 验证，流式模式也通过 get 验证，然后继续 Workspace get/create/reload。现有 Workspace 可省略 workflow_name 按名字打开。内容分级策略、显示顺序、权限与筛选条件由产品拥有。

## Session catalog 流式合同

配置 `catalog_sink` 时，Session 对配置的 `tags` 做一次 AND 查询，用 cursor 和每页最多 8 条分页读取 Workflow，使小型 response storage 也能容纳含多语言 metadata 的页面。`catalog_bytes` 只容纳一页响应或一次 Workspace RPC，和条目总数无关；每页的结构与字符串只在 `H2_GIZCLAW_CATALOG_PAGE` 回调返回前有效。回调在调用 register/refresh 的任务上同步执行，不得重入同一个 Session。首次有效页后发 BEGIN，随后发送 PAGE；全部页和 Profile 名称、revision 一致且未取消时发 COMMIT。RPC、格式、超时、取消或 sink 失败后发 ABORT，调用方须丢弃临时文件并保留旧发布文件。sink 应验证自身文件大小、索引、重复条目和持久化结果；Session 的 `workflow_count` 只在 COMMIT 成功后更新。连续超过 16 个空的续页视为异常。注册仍自动刷新；Catalog 失败不撤销已完成的注册。

流式模式不保存完整 catalog，`catalog_copy` 返回 `UNSUPPORTED`。按 `(workflow_name, workspace_name[, parameters])` 选择时，Session 用 Workflow get 验证返回的 name 与 Profile revision，再按原有 Workspace get/create/reload 合同执行；现有 Workspace 也可只按名称选择。产品应从自己的文件读取显示窗口，Session 不负责产品文件路径或持久化。`max_workflows` 在流式模式不用，`retain_catalog_buffer` 必须为 false。

### 兼容的完整 catalog 模式

Session 的 `catalog_bytes` 是完整 catalog 解码和单次 Workspace RPC response storage 各自的容量。刷新成功后，catalog 的条目和字符串仍引用该 storage；Workspace preparation 必须使用另一块 scratch，不能覆盖已发布的 catalog。

`h2_gizclaw_session_config_t.retain_catalog_buffer` 默认为 false，保持按操作分配的行为：刷新分配新 catalog storage，成功后释放旧 catalog，失败时释放新 storage；Workspace preparation 的 scratch 在操作结束时释放。设置为 true 时，create 从 `retained_allocator` 分别预分配两块 `catalog_bytes`（该字段为 NULL 时回退到 `mem`），每块只分配一次；任一分配失败即返回 `H2_PAL_ERR_NO_MEMORY`，释放已取得的资源并保持输出 Session 为 NULL。该模式在 Session 生命周期内保留 `2 × catalog_bytes`（例如容量为 256 KiB 时保留 512 KiB），让长期运行后的堆碎片不再影响这两块大缓冲的取得；其他 RPC、transport 和音频分配仍可能失败。

可选的 `const h2_pal_mem_api_t *retained_allocator` 仅用于保留模式的 catalog 和 scratch，调用方可将这两块长期存活的大缓冲放在独立于碎片敏感 arena 的内存中。Session 本体、同步对象及其他 Session 分配仍使用 `mem`；`retain_catalog_buffer` 为 false 时忽略 `retained_allocator`。两个 allocator 及其上下文由调用方持有，生命周期须覆盖成功的 Session destroy；创建回滚和 destroy 都通过分配时的同一 allocator 释放缓冲。

保留模式下，刷新只写 scratch，成功后在 Session mutex 内交换 catalog 和 scratch；失败不发布部分结果，并沿用 catalog FAILED、不可复制为有效数据的语义。Workspace select、自动刷新和版本不匹配后的重试复用 scratch，每次 response storage 从 used = 0 开始。现有 busy、等待和 deadline 规则继续保证独占使用：register/refresh 遇到 preparation 返回 BUSY，select/conversation 在期限内等待，复制 catalog 仍由 mutex 保护。操作失败、取消或 close 不释放保留缓冲；调用方完成 join、释放 Conversation 并成功 destroy 后，两块缓冲才返回原 allocator。其他缓冲区的容量和生命周期不受此开关影响。

## Connection transport 生命周期

`h2_gizclaw_client_connect()` 在返回成功前必须注册 Opus 上下行 media，并建立 connection-scoped Direct Packet 和 Peer Event channel。`libs/gizclaw` 在 connect 前注册 PAL WebRTC media extension；调用方不能把 media 当作可选能力，也不能在连接已建立后替换 extension。RPC 和 HTTP service channel 按调用动态创建，不属于这组固定 transport。

本地 DataChannel 的 owner 登记与 outbound 实际并发量一起增长，不使用 SDK 的 inbound RPC 数量限制。适配层在调用 PAL 创建 channel 前预留登记空间；分配失败时返回错误，不创建无法发送或关闭的孤立 handle。每项保留 channel identity、发送背压和本地关闭状态：显式 close 立即撤销 send/close 权限，但保留来源身份直到 owned terminal event 被派发，已排队的 OPEN 因而不会把关闭中的本地 channel 误认成 Server 反向 RPC。登记节点地址固定，通过原子链表头和 live handle 发布供其它 client 的 owner 查询；协议状态仍只由该 client 的 owner 访问。空节点复用，peer close 清空身份，client deinit 在停止自身协议工作后释放节点和原子存储。

`//libs/gizclaw:h2_gizclaw_channel_lifecycle_test` 经过真实 GizClaw WebRTC 适配层、H2Peer、H2SCTP 与本地 Pion，由两个独立 owner 同时驱动两个 client，各自在同一 Peer 上保留六条 channel，再交替执行 180 次收发关闭与 OPEN 派发前取消，验证 SID 实际复用、关闭后远端 channel 数回到基线、没有误报 remote channel，以及登记分配失败和最终 allocator 清理。该 Host 互通回归不替代 ESP 设备长时业务测试。

Peer Event 的物理 service channel 由 SDK connection 持有，唯一 access handle 由 `h2_gizclaw_client` 从 connect 成功一直保留到连接关闭。Conversation 只取得该 handle 的逻辑 lease；同一 client 同时只能有一个 conversation。每次 lease 使用 connection 内单调递增且唯一的 input stream ID，只用于我们自己的输入（BOS、READY、EOS 与服务端对它的拒绝）。服务端下发的 stream ID、BOS、EOS 设备不看：下行音频由 Service 级下行通道收到即解码写入绑定的 Track，不属于任何一轮输入，也不经 event 复制；下行流结束（包括 `STREAM_INTERRUPTED` 等错误码）都不是 conversation 错误。conversation event 只有输入活跃期间转发的文本和服务端拒绝本轮输入的 `ERROR`；push-to-talk 一轮在输入结束发出后完成，realtime 持续到挂断。Conversation deinit 只释放逻辑 lease，不释放 client access handle，也不关闭物理 channel；所有 conversation handle 必须先于 client deinit 释放。Direct Packet、Peer Event 或 Opus transport 意外关闭时，`h2_gizclaw_client_poll()` 返回 `H2_PAL_ERR_CLOSED`，调用方必须 close、deinit 并重建完整 client，不能只重开单条 transport。Peer 仍显示 connected 但已不送达任何内容时同样按关闭处理：一个 RPC 以超时结束且期间没有收到任何 DataChannel message 或 Opus frame，下一次 poll 记录 WARN `stage=peer_unresponsive` 并返回 `H2_PAL_ERR_CLOSED`，恢复方式相同（判定规则见 [GizClaw transport](/apps/gizclaw/transport#peer-无响应判定)）。

Conversation 上行先发送 BOS，再等待当前 input stream 的 `AUDIO_INPUT_READY`；发送成功不代表服务端已完成授权。确认前不采集或编码 PCM、不发送 Opus，但事件队列和下行处理继续推进，避免业务事件占住队列后阻塞 READY。READY 前只有显式关联当前 input stream 的事件能够绑定回复 route，允许服务端提前拒绝当前输入；取消的旧输入或独立旧 reply 的迟到事件不得污染新会话。READY 后允许服务端生成的独立 response ID。等待沿用有界超时，取消仍清理已发送的 BOS；错误 stream、已取消或已提交输入的确认不能重新放行。READY 不参与下行 response-local route 绑定。

当前固定的 C SDK 已包含此协议：`generated/events/peer_event.pb.h` 定义 event type 9、payload tag 18 和 `AudioInputReady.stream_id[129]`。

PAL WebRTC 的 `CLOSED` 和 `ERROR` callback 只提供 callback 期间有效的 borrowed DataChannel handle，backend 可以在 callback 返回后释放它。GizClaw C SDK 必须在 callback 返回前清空 matching service、active RPC、Direct Packet 和 inbound alias；Peer Event 继续保留 SDK-owned service state 供普通 client cleanup 使用，但不再保留 DataChannel alias。后续 request completion、cancellation、client close 或 deinit 只能释放 SDK state，不能再次把已消费的 handle 传给 PAL `channel_close`。显式 close 先于终态 callback 时仍只向 PAL 发起一次 close。

## 设备 provider

SDK 拥有 request-scoped Peer RPC channel、`client.tool.v0.invoke`（135）的封装与内层回复包装、`client.tool.v0.list`（136）的工具发现，以及 `client.rpc.methods.list`（137）的数字协议列表。GizOS 向 SDK 注册有实际 handler 的 `ClientTool`，不维护另一份方法名称表。MHS read/write（133/134）始终安装；未知 key 返回 `NOT_FOUND`。旧的独立设备方法不再是 RPC registry 的成员，不提供别名或版本探测。

`h2_gizclaw_config_t` 借用 `tool_handlers` 到 Client/Service deinit。每项声明 numeric `h2_gizclaw_tool_t`、同步 invoke callback 与 user。Handler 接收该工具的内层 Protobuf，返回同一工具的内层结果；SDK 负责外层 tool/v0 envelope。空 callback、重复或未知 tool、超过 registry 容量的表使初始化失败。Service 根据 PAL、产品 vtable 和设备信息配置安装内置工具；产品可以注册 `DEVICE_FIND` 和 `SOCIAL_PING`，不能覆盖 Service 拥有的工具。直接使用 standalone Client 的调用方可以注册任一有效 ClientTool。表和 user 必须在整个借用期保持有效，运行中不增删。

| 操作 | Library owner | 配置来源 |
| --- | --- | --- |
| `info.get`、`identifiers.get`、`device.status.get` | 身份与状态编码、字段校验 | manufacturer/model/hardware_revision/serial、get_facts、Audio |
| `sound.play`、七个 `audioplayer.*` | 下载、Ogg/Opus、MP3 与 WAV 解码与播放器 | Audio、resolve_sound_url、HTTP |
| `wifi.scan/connect/saved.list/saved.forget` | 参数解码、PAL 操作与凭据清理 | Wi-Fi 与 Wi-Fi Settings |
| `firmware.update` | 元数据、下载与 OTA Stage | HTTP 与 ota vtable |
| `device.reboot/factory_reset`、`run.workspace.set` | 参数校验、回复完成后的交接 | Power 或对应 request hook |
| `device.find`、`social.ping` | 产品 handler | tool_handlers |

重启、切网、OTA、恢复出厂和 Workspace 切换仍在本地 RPC 回复发送完成后交给 `$gizclaw/device` task。回复失败或 Service 停止会取消尚未执行的动作。`request_reboot`、`request_factory_reset`、`request_run_workspace_set` 只能复制参数、投递到产品 owner 并立即返回，不得阻塞或停止/销毁 Service。Workspace 的 Session、Conversation 和已确认参数属于 App；App 可把 kickoff 映射为 initiative AGENT 与 ON_RELOAD。成功回复仅表示请求被接受。

传入 `audio` PAL 即启用 Ogg/Opus、MP3 与 WAV 播放器，格式只按文件开头的字节识别（见 [GizClaw 音频](/apps/gizclaw/audio#音频格式)）。`audio_buffer_bytes` 设置压缩数据环形缓冲容量（默认 64 KiB），`audio_prebuffer_bytes` 设置起播和缺数据后的预缓冲量（默认 min(16 KiB, 缓冲容量)）。HTTP task 和播放 task 并行，缓冲满时通过背压暂停读取，边下载边解析 Ogg page / MP3 帧 / WAV 采样并解码，不限制整首音频长度。短音频在下载结束后使用已有数据起播；持续缺数据超时会取消下载并上报错误。解码器保留一个最大 65,307 字节 Ogg page，跨页 packet 上限 64 KiB，独立于环形缓冲。超过 64 KiB 的 OpusTags（例如内嵌大封面）只校验开头的 `OpusTags` 魔数，其余字节随读随弃，不占额外内存；音频包仍受 64 KiB 上限约束。MP3 解码器（dr_mp3 帧解码器加库内分帧）约 41 KiB，WAV 约 15 KiB，都远小于 Ogg 的 page 缓冲；MP3 与 WAV 混成单声道后重采样到 16 kHz。以 16 kHz mono PCM16LE 写入 PAL Audio track，按 PAL 报告的帧大小拼帧，末帧补零不计入播放进度。不支持 Vorbis、AAC、FLAC、Layer I/II 与 free-format MP3，其它内容以 `UNSUPPORTED` 失败。库只关闭自己的 track，不关闭共享 speaker。


Wi-Fi provisioning 明确调用 PAL `connect_and_save`；Library 不维护第二份网络配置。音频与本地控制使用同一 Runtime proxy。`speaker_acquire`/`speaker_release` 必须成对提供；播放前 acquire，Track 关闭后在每条退出路径 release，失败、取消和停止也遵守此配对。没有 hook 时播放前 start_speaker，结束不主动 stop_speaker。`sound.play` 以 16 kHz mono PCM16 按 duration_ms 截断，短声音只完整播放一次。

### MHS 静态状态表

`h2_gizclaw_config_t.mhs_states` 是初始化时借用到 deinit 的 `const h2_gizclaw_mhs_state_t[]`。每项拥有唯一 `(device_id, state)`、类型、必需 read、可选 check/write 与 user。Key 使用 `[a-z][a-z0-9]*([.-][a-z0-9]+)*`，最大 64 ASCII 字节；初始化拒绝非法 key、重复、未知类型或缺少 read。没有 write 的项只读。所有 callback 在 RPC owner 上有界执行，不能 sleep、停止或销毁 Service；out_value 的类型与零值由 Library 初始化，callback 返回实际数据。

MHS 一批有 1–32 个唯一 key，顺序保持不变；bool、int、double、UTF-8 string/enum 使用独立类型，false、0 和空字符串都是有效值。Int 限于 JSON safe range ±9007199254740991，double 必须有限，string 最大 256 字节且不含 NUL。Library 校验原始 wire 文本，防止 nanopb C string 截断 key/value。空批次、重复、类型不符、写只读项或非法值返回 INVALID_ARGUMENT，未知项返回 NOT_FOUND，check 的 INVALID_STATE 返回 FAILED_PRECONDITION。

写入先校验整批 key/type/权限，再运行全部 check，最后应用。预检失败不会调用任何 write；write 回报 clamp/round 后实际生效的值。应用中途失败不回滚已改变的硬件，调用方需要重新 read 确定结果。大型 request/reply 使用 allocator，避免占用嵌入式 task stack；返回值只在应答期间借用，随后释放。

| 内置 key | 类型 | 可用条件和行为 |
| --- | --- | --- |
| `speaker.main/volume` | int，0–100 | Audio 支持读音量；支持写音量时可写 |
| `speaker.main/muted` | bool | 与 volume 使用同一能力；同批音量与静音合并成一次硬件应用 |
| `wifi.main/connected` | bool，只读 | Wi-Fi get_status，CONNECTED/GOT_IP 为 true |
| `wifi.main/ssid` | string，只读 | 当前 SSID，必须是合法 UTF-8 文本 |
| `wifi.main/rssi-dbm` | int，只读 | PAL 报告的 RSSI |
| `wifi.main/ip`、`wifi.main/bssid` | string，只读 | PAL 存在对应值时编码，否则空字符串 |

Service 从可用 PAL 自动安装这些 state，并拒绝产品表的同名 key。只有原始 Audio PAL 时，Library 只能报告有效音量（静音时为 0）；需要逻辑静音与保留音量的产品注入 Runtime Audio，Library 读写同一 Runtime state。产品通过自己的 state callback 扩展 display/LED/其他设置，Library 不复制 DeviceSettings 或为无法读回的 PAL 伪造状态。RuntimeProfile manifest 归产品，必须与实际设备表一致；注册 state 不会修改或发布 manifest。

### 播放与产品交接

- 本地 `playlist_set` 条目可带 `duration_ms`（0 为未知），只用于 `h2_gizclaw_player_play_index_at` 定位，不作为状态里的时长上报；RPC 推送与 `player_play` 的条目时长为 0。时长已知且起点非零时，下载 task 先发不限长度的 `Range: bytes=0-` 解析文件头，解码器读完文件头（Ogg 两个头包、MP3 的 ID3v2 与首帧、WAV 的 `data` 之前）后取消该请求，因此文件头大小不受限，再发 `Range: bytes=<offset>-`：Ogg 按头之后的字节率从起点前 5 秒，WAV 精确到起点前 64 个采样帧，带 LAME “Info” 标签的 CBR MP3 精确到填满 bit reservoir 所需帧数再加 2 帧之前，落点到起点的每一帧都须符合声明的码率和字节网格；VBR 或无标签 MP3 不发 Range，直接在探测请求上逐帧跳过。以下 page、granule 与 packet 的描述针对 Ogg/Opus。响应头里的 `Content-Range` 在第一个 body 字节处核对：探测请求允许缺失（表示服务器忽略 Range，直接在该 200 响应上跳到起点），续传请求必须精确命名所请求的起始字节和文件末尾，且总长度等于探测时的总长度（中途被替换的文件不会接到旧的文件头上）；结束时 partial 响应必须是 206，字节数等于 `Content-Range` 与 `Content-Length` 声明的长度。解码器从任意字节开始扫描 `OggS`，只接受 CRC 正确、同一 serial、非 BOS 的完整 page；被拒候选里已读的字节原地重扫，超过两个最大 page 仍无可用 page 返回 FORMAT。第一个非 EOS、带 granule 且有 packet 在其上开始的 page 作为锚点，其起点为 granule 减去该 page 上完整 packet 的时长（由 TOC 得出，不解码）；之后预滚 80 ms。结束于起点前 80 ms 之外的 packet 只校验不解码，第一个解码的 packet 前重置 Opus 状态，起点之前的样本丢弃。首个样本的位置与从头播放的计数口径相同，因此 `position_ms` 是 granule 推出的精确值；首个样本前的任何失败（非停止）改用一次普通 GET 顺序跳到起点，文件在起点前结束则该条目在结尾处正常结束。
- `h2_gizclaw_player_rate_set` 的速率存在设备对象的原子变量里，worker 每个变速步长读取一次；只对 music 播放生效，命名音效固定原速。解码后的 PCM 在拼 PAL 帧之前经过 `h2_gizclaw_time_stretch.c`（定点 SOLA，工作缓冲在条目内首次离开 1000 时才分配）。位置按两套计数：拼帧输出字节与其代表的源字节；每写入一帧记录该帧承载的源字节，扣除队列时按最近若干帧各自的源字节扣（队列里可能混有切换前后不同速率产生的帧），因此切换速率时位置不会超前或回退，1000 时与原先的“已提交减队列”相同；结束时位置为 origin 加全部源字节，即真实时长。切回 1000 或条目结束时 flush 先输出携带的重叠段，再原样接续其后的源样本。
- 播放列表支持最多 32 项、读取/替换/追加、从指定索引播放、停止和 off/one/all
  循环模式。失败的列表校验保留旧列表和播放；停止或替换取消在途下载/播放。
  播放中进度按已写入 PCM 扣除队列容量及一个在途帧保守估算，结束时 drain 后
  校准到全部源采样；不逐帧 drain，避免插入静音。状态变化及约每秒进度通过 telemetry
  异步提交，不阻塞播放等待网络上报。
- `h2_gizclaw_vtable_t` 只补 PAL 缺少的产品事实、命名提示音到 HTTPS 音频（Ogg/Opus、MP3 或 WAV）URL 的解析、H2Loader Stage begin/write/finish/abort/activate 与产品动作交接。get_facts 在 RPC owner 上快速返回，提示音和 Stage 操作在设备 task 上执行。request_reboot 未设置时使用 Power PAL，设置后由产品 owner 完成延时、有序关机和重启。Factory reset 与 Workspace set 不提供库内回退路径。
- `get_facts` 的 `imei_count` / `imeis` 上报设备 modem IMEI，最多 `H2_GIZCLAW_DEVICE_IMEI_MAX` 个。产品必须返回已缓存的号码，不得在回调里发 AT 命令读取 modem；还没读到时返回 `imei_count = 0`。每项 `digits` 必须是 15 位 ASCII 十进制数字加 NUL，`name` 是可选的槽位标签，为内联缓冲区，必须在 `H2_GIZCLAW_DEVICE_IMEI_NAME_MAX` 内以 NUL 结尾，空串表示不命名；facts 整体 按值复制，回调返回后库不持有任何指向产品内存的指针。库在 `identifiers.get` 工具 里按 `tac = 前 8 位`、`serial = 后 7 位` 拆分编码，与 Server 的 by-imei 索引一致。 任一项不合法整个回复失败，不发送部分列表；`get_facts` 失败或没有配置 vtable 时 回复退化为仅 `sn`，不报错。IMEI 属于个人数据，只出现在 RPC 回复里，不进入 `h2_pal_log` 输出与 trace 字符串。
- OTA 使用明确的 `firmware_channel`，允许 RPC 覆盖 channel 并附带期望 SHA-256。
  库获取元数据并通过 PAL HTTP 下载；Stage backend 必须验证 package 的长度、
  SHA-256、board/target 和 manifest，验证通过才能发布 Stage。库上报 started、
  downloading、failed；安装后新固件核对运行身份，使用保存的 update_id 上报 succeeded。

C SDK 的 provider 合同仍是同步回复，所以 Wi-Fi scan 在 RPC owner 上执行有界 PAL
扫描（默认 5 秒、最多 30 秒）。下载、音频解码/播放和 OTA 均在独立设备 task 上执行。
长扫描期间会占用 RPC owner；不能用一个提前 ACK 冒充扫描结果。

应用主动上报时，`h2_gizclaw_telemetry_observation_t` 增加 `AUDIOPLAYER`、`OTA` 和
`ACTIVITY`。OTA frame 必须只包含一条 OTA observation，以映射 SDK 独立的 OTA frame
API；其余 observation 继续使用原有批量 frame。上报成功仅表示本地 transport 接受。

`h2_gizclaw_telemetry_kind_t` 的编号是库自有的，不是 SDK 的 observation kind：`OTA`
在 SDK 侧没有对应的 observation（走独立 frame API），而 SDK 把 6 用于 `ACTIVITY`，
所以 `h2_gizclaw_telemetry.c` 用一个显式 switch 在两套编号之间转换，不做强制转换。
往 `h2_gizclaw_telemetry_kind_t` 追加新 kind 时必须同时补这个映射。

`ACTIVITY` observation 上报设备当前在用的功能：`activity` 是 1 到
`H2_GIZCLAW_TELEMETRY_ACTIVITY_ID_MAX`（32）字节、首字节 `[a-z0-9]`、其余
`[a-z0-9_.-]` 的机器可读 id（如 `idle`、`chat`、`audioplayer`、`ota`），`detail` 是
可选的展示文本，最多 `H2_GIZCLAW_TELEMETRY_ACTIVITY_DETAIL_MAX`（128）字节且不得携带
敏感信息。activity 与 detail 作为一个整体合并，因此不带 detail 的观测会清掉上一个
activity 留下的 detail；空 span 与 `has_detail` 为 false 等价，不写入字段也不发送空
串。长度或字符集不合法时 `h2_gizclaw_req_create_telemetry_send()` 返回
`H2_PAL_ERR_INVALID_ARG` 并且不产生网络请求，同时向 `h2_pal_log` 写一条 WARN，内容只
有字段名和长度——`detail` 由产品决定内容，库不替它判断可以打印，因此 id 和 detail 都
不进日志与 trace。两个字符串只在创建请求期间借用，创建时复制进请求自有存储。

`h2_gizclaw_telemetry_system_t` 的 `firmware_version` 现在会被服务端采用并投影到
`PeerStatus.firmware_version`（此前被校验后丢弃），产品据此上报固件版本号；字段本身
没有变化，仍受 `H2_GIZCLAW_TELEMETRY_VERSION_MAX` 约束。

`h2_gizclaw_telemetry_network_t` 的 network observation 还可携带蜂窝身份
`has_imei` / `imei` 与 `has_imsi` / `imsi`，编码为 `NetworkObservation` 的
optional string（tag 6 / 7），服务端据此把观测归到 by-imei 索引的 modem 身份上。
`imei` 恰好 `H2_GIZCLAW_TELEMETRY_IMEI_LEN`（15）位 ASCII 十进制数字，`imsi` 为
`H2_GIZCLAW_TELEMETRY_IMSI_MIN_LEN` 到 `H2_GIZCLAW_TELEMETRY_IMSI_MAX_LEN`（6 到
15）位；空 span 与 `has_*` 为 false 等价，不写入字段也不发送空串。长度、字符集
不合法，或 `rat` 忽略大小写等于 `wifi` 时仍携带任一字段，
`h2_gizclaw_req_create_telemetry_send()` 返回 `H2_PAL_ERR_INVALID_ARG` 并且不产生
网络请求，与服务端的拒绝规则一致。两个字符串只在创建请求期间借用，创建时复制进
请求自有存储。IMEI / IMSI 属于个人数据，只出现在编码后的 telemetry payload 里，
不进入 `h2_pal_log` 输出与 trace 字符串。

设备身份可用 `h2_gizclaw_rpc_api_key_create()` 创建 HTTP API key，用 `h2_gizclaw_rpc_api_key_revoke()` 撤销；也提供相应 create/do/wait/parse/release 接口。返回的 secret 由调用者管理，不应写入日志。

Provider 在 `h2_gizclaw_client_poll()` 所在线程同步运行。上游 C SDK 要求 provider 在返回成功前恰好提交一次 response；GizOS adapter 将这个 responder 细节封装为同步 `out_response`，并在 provider 返回后立即把结果交回上游 responder。Request payload、response payload 和 error message 都是 protobuf byte view：输入只在 callback 期间有效，输出必须在 callback 返回后保持有效，直到 adapter 消费返回的响应；不能返回栈上 buffer。

设备主动调用 Server 的 unary 或 server-streaming RPC 与 Server 反向调用 Client provider 是两个方向的 contract。前者由 generic RPC call API 发起；后者只能从 poll 驱动的 provider 入口处理，不能由 UI callback 直接执行，也不能跨线程保留 borrowed payload。产品侧的 state、effect command 和 main-loop 投影规则见 [GizClaw 状态与请求](/apps/gizclaw/state)。

## Workspace 参数补丁

`h2_gizclaw_workspace_parameters_patch_t` 是 `workspace.parameters.set`（110）和
`server.run.workspace.reload-with-options`（120）共用的补丁：缺席的成员保留服务端已存
的值，值在创建请求时复制，不借用调用方的补丁存储。

`tts_speech_rate_percent` 缩放服务端为 agent 回复合成的语音，取
`H2_GIZCLAW_WORKSPACE_TTS_SPEECH_RATE_MIN_PERCENT` 到
`H2_GIZCLAW_WORKSPACE_TTS_SPEECH_RATE_MAX_PERCENT`（50 到 200，100 为正常），缺席表示
沿用 Workflow 配置，下一次 reload 生效。语速必须在合成侧生效：下行音频是实时到达的，
在设备侧放慢播放只会让缓冲和延迟持续增长。越界值在创建请求时就返回
`H2_PAL_ERR_INVALID_ARG`、不产生网络请求，与服务端的 `INVALID_ARGUMENT` 一致；system
（SFU）Workspace 接受合法值但不做任何事，所以一份补丁可以发给任何 Workspace。设备只需
在每次 `reload-with-options` 里把语速和 `input` 一起带上，不需要额外调用。

只改语速的补丁是完整的补丁：`h2_gizclaw_session_select()` 会因为已确认参数不同而重新
reload，不会被当成“参数没变”跳过，成功后语速出现在
`h2_gizclaw_session_snapshot().parameters` 里。

### 安全围栏

可选 `has_safety_fence_level` / `safety_fence_level` 保存 RuntimeProfile 自定义的档位标识符。设备先从 `server.workflow.list` 响应的 `safety_fences` 发现当前 Profile 支持的 name 与展示名；列表不下发 prompt。标识符匹配 `^[a-z][a-z0-9_-]{0,63}$`，写入时由 Server 保存，reload 时验证它属于当前 Profile。两个写入 RPC 均使用 SDK 0.23.1 的字符串字段（Workspace patch tag 5）；旧 enum wire tag 已保留，不能再发送。显式空值或格式错误在创建请求前返回 `H2_PAL_ERR_INVALID_ARG`，不产生 RPC。absent 忽略数组存储值并保留服务端已有档位；只包含围栏的 patch 也有效。请求复制 patch，调用方不需要保留原始存储。

`parameters.set` 保存档位，下一次 reload 才应用。`reload-with-options` 的保存和 reload 不是同一事务：缺少 Profile 文案等错误可以发生在档位已经保存之后，失败不回滚服务端存储。Session 只在目标 Workspace 的 RUNNING 激活确认后合并 present 档位；失败、错误名称、非 RUNNING、格式错误、超时或关闭后的迟到响应均不能把请求档位发布为 confirmed。FAILED 状态保留此前确认值用于显示，不表示服务端仍存该值。

同一 Workspace 上的普通省略保留 confirmed 档位；成功切换到另一 Workspace 时，旧 Workspace 的 confirmed 档位失效。同步 `parameters.set` 与 Session 的选择、reload 共享串行请求槽，但不停止当前对话，也不提前改变已确认档位。一次可能已发送的围栏 set 或失败的围栏 reload 后，后续省略档位的成功 reload 只能把围栏标为 unknown：本库没有读取服务端 typed Workspace parameters，不能从旧快照推断最新保存值。再次显式设置档位且 reload 成功才恢复确认；Session 不会因为请求恰好等于旧 confirmed 值而跳过必要的 reload。

实际围栏文案属于 RuntimeProfile 的 `spec.safety_fences.<id>.prompt`，每档为独立完整的 1–4096 字符提示词，不继承其他档位。Flowcraft 的 Workflow 必须引用 `${board.safety_fence}`，Eino 必须绑定 `input.safety_fence`，Realtime Workflow 必须在 instructions 中引用 `${input.safety_fence}`。缺少所选 Profile 条目时，支持注入的 driver reload 明确失败；没有引用变量的 Workflow 不会注入围栏。ASTTranslate 保存合法值但不注入，SFU 接受合法值但 no-op。RPC 成功和 Session confirmed patch 都不是内容审核效果或产品档位回读的证明，设备不执行替代性的本地关键词过滤。

公共协议不固定档位数量、顺序或文案，也不定义产品年龄过滤。H106 的四档必须在产品 RuntimeProfile 中逐档配置完整文案，并只展示 `safety_fences` 中真正可用的选项；年龄选择另行保存，不映射成围栏 ID。

围栏测试覆盖两种 RPC 的非空 patch 组合、字符串 wire tag、复制 ownership、非法标识符零 RPC，以及生产 Session → request → nanopb → response parse → snapshot 的参数流和错误路径。Workflow list 解出当前 Profile 的可选档位并拒绝重复或非法 ID。真实服务器验收需使用已配置的隔离测试 Profile，验证档位发现、保存、遗漏保留、移除已选档位后的 reload 失败且保存未回滚、修复后恢复和 Workflow 注入，记录 endpoint、Profile revision、服务端版本与清理结果。

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

`h2_gizclaw_req_create_debug_get()` 使用 `server.runtime.get` 读取服务端当前
持久化的 debug mode（`Runtime.debug_mode`），供页面在用户改动前显示已确认的
模式；服务端从未存过模式时响应成功但 `mode` 为空，页面不显示“当前”。解析用
`resp_parse_debug_get`，生命周期与 debug_set 相同。

产品通常不需要自己持有这些请求：Service 维护 `h2_gizclaw_debug_snapshot_t`
（`known`、`mode`、`busy`、`last_result`、`revision`），`h2_gizclaw_debug_refresh()`
和 `h2_gizclaw_debug_set_mode()` 发起库自有的 get/put，完成回调在 poll owner 上把
服务端确认的模式写入快照；产品只在主循环读 `h2_gizclaw_debug_snapshot()` 投影，
进页面时调用 refresh，确认时调用 set_mode。同一时刻只允许一个在途请求（BUSY），
Service stop 会取消并丢弃在途请求，快照保留最后确认的模式。

## 上游 API 同步

`@gizclaw_c_sdk//:gizclaw_core` 中的 RPC registry 与 protobuf payload 是 wire contract 的生成结果。RPC schema 更新时，先把 `MODULE.bazel` 中 `gizclaw_c_sdk` 的 `bazel_dep` 版本更新到同一个规范版本，再同步已有 `libs/gizclaw` stable wrapper；不能只修改手写 method number、复制旧 protobuf struct，或只更新产品文档。没有 GizOS-owned domain/lifecycle 语义的 RPC（例如 Firmware metadata）直接使用 generic RPC API 与 pinned generated schema，不为相同字段再增加一层 typed wrapper。GizOS 中公开的 RPC method 常量通过 compile-time assertion 与上游 registry 对齐，registry 再次漂移时必须使 build 失败。

Module 自带 Bazel targets、生成代码和精确的 nanopb runtime，不再注入 BUILD overlay、单独解析 nanopb 或维护 SDK source patch。`gizclaw_c_sdk` 发布在 GizClaw 自有的 Bazel registry（`https://static-volc.gizclaw.com/bazel/`）上，`.bazelrc` 通过 `--registry` 把它排在 `https://bcr.bazel.build` 之后，只有 BCR 不提供的 module 才落到它；因为它是正常的 Bzlmod module，下游 consumer 直接 `bazel_dep` 即可，不需要 `archive_override` 或手写 integrity。具体版本以 `MODULE.bazel` 中的 `bazel_dep(name = "gizclaw_c_sdk", ...)` 声明为准，archive 的 SRI 校验由 registry 的 `source.json` 提供。

Wire message 使用 `name` / `*_name`。GizOS wrapper 将 Peer-addressable resource 继续公开为 name；将 occurrence、relationship、history 和 ledger 的 wire name 逐字节映射到既有 public `id` / `*_id`，不做 trim、派生、翻译或 storage-ID 替换。技术性 transport request ID、idempotency key 和 `gear_id` 不属于这层业务 identity 映射：

| 资源 | Peer selector / projection |
| --- | --- |
| Registration / Firmware / Speech | `runtime_profile_name`；Firmware 由 channel 选择；Speech 使用 `*_model_name` |
| Workflow / Workspace | Workflow `name`、Workspace `name` / `workflow_name`；history public `id` / `history_id` 映射 wire `name` / `history_name` |
| Contact | immutable caller-local `name`、mutable `display_name` |
| Friend / FriendGroup | Friend/member/history public ID 映射 wire name；FriendGroup `name` / `friend_group_name` 与独立 display name 保持 name 语义 |

Runtime Profile alias（包括 Workflow `name` 与 Workspace `workflow_name`）最长 63 字节，由 `.` 分隔的 lowercase kebab-case segment 组成；完整 alias 是不拆分、 不归一化的 opaque key。`libs/gizclaw` 在 catalog decode、Workflow get 和 Workspace create 边界使用该 grammar。Workspace `name`、history public ID 和其它非 Runtime Profile identifier 继续使用各自 contract，不能因为 alias 支持 `.` 而一并放宽。

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

外部设备控制 E2E 使用 `/device/tool/v0/invoke` 与 `/device/mhs/v0/states`，其 RuntimeProfile 必须预先声明可写的 `speaker.main/volume` 和 `speaker.main/muted`。本地模拟测试验证协议与生命周期，真实 Server/设备互通单独记录。

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

live suite 还必须验证 pinned GizClaw C SDK 的 single-client 并发与长期 channel 回收。concurrency suite 在同一个 active client 上执行 32 批请求，每批先启动六个 Ping handle，再等待各自终态；唯一 serialized poll owner 推进协议。每批记录六个不同 stream ID、六个成功 result、零残留 channel 和恢复 Ping，并等待恢复 Ping 的关闭事件后再开始下一批。最终必须完成 192 个批内请求及 32 个恢复 Ping，不通过重建 client 或 Peer 规避复用；应用线程不直接并发调用共享 client 的协议 poll。
社交 fixture 的 helper Peer 只用于 Friend/FriendGroup 建模。除此之外，测试还必须验证两个 Peer 可各自使用相同 Workspace、Contact 和 FriendGroup
name 且互不可见，同一 Peer reconnect 后可恢复原 Workspace/history，并覆盖 method
95 的 metadata、stream byte count 与清理失败路径。Linux x86_64 与 macOS arm64 都是
merge 前的外部服务合同证据。

测试创建的 Workspace 通过 `h2_gizclaw_rpc_workspace_delete()` 删除；返回的
snapshot 使用调用方 response storage。所有业务资源清理完成后才请求 Peer 删除。


### 设备控制回复后的本地动作

产品 provider 可以在成功响应中设置 `on_complete` 与 `complete_user`。GizOS 将其关联到当前入站 RPC 通道，在 SDK 接受响应写入并正常关闭该通道后，从 poll owner 调用一次。普通 poll 返回成功不表示该通道已经完成；非终态的 WOULD_BLOCK、TIMEOUT 或其他 poll error 也不能取消尚未关闭的通道。成功必须同时具备该通道响应 EOS 已被 PAL 接受、SDK 本地关闭的证据；adapter 按通道跟踪已接受的帧，支持跨 send 分片与背压重试。同一次 poll 中其他通道超时或失败，不会撤销已成功关闭的响应。未发完 EOS 的通道即使本地关闭也只能报告失败。发送阻塞时继续保留动作，编码错误、发送失败、远端取消和客户端停止则以非 OK 结果撤销。关闭或销毁客户端时，在对应 owner 上释放未完成动作；注册失败可能在 provider 内同步返回失败通知。每个 client 最多保留四个完成回调；满容量、缺少当前入站通道或错误响应附带回调时，新回调同步收到 INVALID_STATE，响应不提交，已有槽位不受影响。

这是本地发送生命周期，不是对端收到或处理回复的确认，不新增网络消息，也不更改 GizClaw 0.15.3 SDK。回调只能向产品 owner 发布待执行动作，不能阻塞或重入 client API。产品参数必须复制到自身状态，生命周期覆盖完成或取消；不能保留借用请求、栈上的响应或已释放的 user。H106 的重启和临时切网消费此路径，OTA 是否支持仍由产品决定。

## Conversation 文字输入

Session 选好目标 Workspace 并创建 Conversation 路由后，调用
`h2_gizclaw_session_send_text(session, text)` 可把一整段文字作为用户输入交给 Agent，
例如主动请求继续内容。文字输入与 audio_start/audio_end 一样是 Session 操作：使用
Session 的应用只能通过它发送文字，Session 据此维护会话状态。该 API 不切换 Workspace；
Peer Event 路由到连接上当前激活的 Workspace。文本按 UTF-8 字节计长，接受 1–4096 字节，
不要求调用方补 NUL，拒绝嵌入 NUL 和非法 UTF-8；成功 admission 前复制文本，返回后可
立即复用原缓冲区。

发送沿用音频输入的纯控制 `BOS(kind=UNSPECIFIED, mime_type="")`，随后只发一条
`TEXT_DONE`。两条事件使用同一连接内新分配的 `demo-<sequence>` stream ID，label 沿用
现有输入的 `demo-home`，事件 sequence 分别为 0、1，timestamp_unix_ms 均为 0。
这些标识仅关联输入，不选择 Workspace，也不决定角色。服务端
`pkgs/gizclaw/peer_stream_event.go` 的 `peerStreamEventToChunk` 把 `TEXT_DONE` 转为
`RoleUser` 文本并设置 `EndOfStream=true`；因此不再发额外 EOS，也不发音频 BOS、
不等 AUDIO_INPUT_READY、不生成 Opus。协议依据为 `api/proto/events/peer_event.proto`
及服务端 Events Reference 的 Logical stream lifecycle。

空/超长/非法文本返回 `INVALID_ARG`。Session 已关闭或正在准备、Workspace 未 READY、
尚无 Conversation 路由或 Service 未 start 时返回 `INVALID_STATE`；Service 停止中返回
`CLOSED`。录音/通话输入打开，或上一输入（音频或文字）尚未 completion 时返回 `BUSY`，
不打断当前输入。队列满返回 `WOULD_BLOCK`；分配失败返回 `NO_MEMORY`。Service 已 start
但仍连接中时，与音频 start 一样允许排队，连接或发送失败通过 completion 报告。

受理后 Session 的 conversation 状态进入 `WAITING`（PTT 与 Realtime Workspace 相同），
`conversation_input_open` 保持 false；下行音频到达、`H2_GIZCLAW_SESSION_WAIT_MS` 内
没有声音或输入失败时回到 `IDLE`，失败写入 `last_error` 与 `error_stage`。completion
之前 Session 仍占用路由：`h2_gizclaw_session_conversation_release()` 不释放；
`h2_gizclaw_session_audio_start()`、切换 Workspace 或删除当前 Workspace 会先取消
待发文字并等待取消分发，与取消上一段音频输入相同。Realtime Workspace 中文字
completion 不会自动开始通话。

调用方不执行网络 I/O。Service 网络任务按序发送并在背压时重试，deadline 沿用
connect_timeout_ms（未设置时 30 秒）。每个已接受的输入通过原有 `service_poll()`
completion 恰好报告一次发送成功、失败或取消；同步 admission 错误不产生 completion。
成功仅表示输入已发出，不保证服务端接受或 Agent 已回复。

文字输入不需要可读 PCM Track。服务端音频仍通过连接级下行通道播放，即使文字输入
已经 completion；服务器文字事件仍遵循原有“输入活跃期间观察”的 callback 合同，
completion 后不延长文字订阅。应用无需新增下行音频处理流程。


## API key 异步状态

扫码绑定等需要异步刷新和撤销的产品使用 `h2_gizclaw_api_key_state_t`：创建时注入借用的 Service、Mem、Sync、Time 和非零 `timeout_ms`，`display_name` 会复制。产品调用 `request_refresh` 登记请求，主循环继续调用 `service_poll` 驱动 completion，再读取 `snapshot` 展示有效 key。无需创建线程、join 或等待 RPC；原有同步 helper 和 request 接口行为不变。

`request_refresh(true)` 在当前 key 有效时先撤销再创建；撤销成功或 `NOT_FOUND` 后擦除旧 key 并提交 create，其它撤销错误保留 `valid && stale` 的旧 key，下一次 `request_refresh(true)` 可重试撤销。创建成功后 key 有效且不 stale，创建失败则失效。busy 期间重复刷新返回 OK，合并到当前链，不叠加请求、清除 revoke-after 标记、不延长截止时间。每次 refresh 的总时限包含排队、连接、撤销和创建；`snapshot` 或 `request_refresh` 检查到期后将当前请求分离为 orphan（不取消），清除 busy 并记录 `H2_PAL_ERR_TIMEOUT`，保留的旧 key 标为 stale。检查到期的 `request_refresh` 随后可开始新一代刷新。

快照只在 mutex 内短读，不发 RPC；包含 key、valid、stale、busy、closed、last_error 和每次可见变化递增的 revision。`request_refresh`、`request_revoke`、`close` 与 `service_poll` 在同一个 owner task 上调用，`snapshot` 也可从其它线程读取并执行到期分离。completion 只在 owner task 串行执行，每个请求的 generation 隔离超时、close 后的迟到结果。初始状态 invalid、stale、idle；close 后保留的 key 仍可读但 stale，last_error 为 CLOSED，新 refresh 返回 CLOSED。

生命周期顺序必须是 `close` → Service stop → `service_poll` drain → `destroy` → Service deinit。close 可在 Service stop 之前调用，立即停止接收刷新和撤销请求，并将在途请求分离为 orphan；destroy 在任何 completion（包括 orphan revoke）尚未分发时返回 BUSY，调用方继续 drain 后重试，不等待 RPC。destroy 需要排除并发读者，会擦除持有的 secret；快照中的 secret 副本由调用方擦除，不能写日志。超时或 close 后迟到的 create 成功响应会立即触发内部 best-effort revoke：解析 name 后立即擦除 secret，不进入快照；迟到的 revoke 或失败 create 无需后续操作。Service stop 后提交 revoke 可能返回 CLOSED，忽略该错误。无法撤销的情况是 Service 已停止或传输先失败导致 create 响应未到设备，设备从未获知 key 的 name；服务端 owner 的 list API 可返回明文 secret，因此未展示不代表凭证无效。状态对象不做持久化、二维码或 URL 格式化，也不属于 resource store 的 kind。

`h2_gizclaw_api_key_state_request_revoke` 非阻塞：idle 且有有效 key 时提交撤销，期间 busy；OK/NOT_FOUND 后擦除 key 并置 valid=false，失败保留 valid+stale 及 last_error。无 key 返回 OK。刷新 busy 时设置 revoke-after，create 成功后直接撤销新 key 而不展示，最终 valid=false，撤销成功 last_error=OK，失败记录错误且不暴露 secret。busy 期间再次 request_refresh 会清除该标记，表示调用方重新需要刷新结果。closed 返回 CLOSED。
