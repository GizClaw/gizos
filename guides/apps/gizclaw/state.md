# GizClaw 状态与请求

`libs/gizclaw` 的 Session 持有连接作用域内的 Runtime Profile 身份、Workflow catalog、 Workspace 准备状态和 Conversation 状态。产品读取公共快照并投影到页面，不再维护另一份 可用于业务决策的 Profile/catalog/Workspace 状态。低层 Service/RPC 仍可独立使用； 选择 Session 的同一 Service 通过 Session 执行注册、catalog 和 Conversation 操作。现有同步 Workspace activate/reload/reload-with-options RPC，以及删除 Session 当前 Workspace 的同步 delete RPC，自动进入 Session 的收尾与状态发布逻辑；低层异步 request 接口仍只处理协议，不能用于绕过 Session 的受管 Workspace 修改。

## 数据与职责

| Session 持有 | 产品持有 |
| --- | --- |
| 注册结果、Profile name/revision | Credential 来源、连接重建策略 |
| 完整 Workflow catalog、加载和失败状态 | 要查询的 collection、必需 Workflow、默认模式 |
| 当前已确认 Workspace、正在准备的目标 | Workspace 命名、用户选择、页面焦点 |
| 对话准备、输入开启、活动和终态 | 按键语义、麦克风与扬声器 pump、错误页面 |

Session 不内置产品 collection、默认 Workflow、命名规则或文件路径。Catalog 保存在 库拥有的有界内存中；产品可保存显示投影，但磁盘上的旧投影不是当前连接的有效凭据。 未注册、加载失败、Workspace 未确认或已有对话时，`can_start` 为 false。 `blocking_reason` 区分阻塞阶段；`error_stage` 和 `last_error` 描述最近操作失败。 它们不声称设备麦克风、网络之外的产品使用限制或 UI 已就绪。

## 准备流程

注册成功立即加载配置要求的 collection，不依赖产品打开页面。分页必须符合容量上限、 有界页数、collection 和唯一 Workflow name 要求，所有页面必须具有同一个 Profile name/revision。只有完整成功才替换 catalog；失败保留已分配的旧数据，但公开读取拒绝 把它当作有效数据。注册成功而 catalog 失败分别记录，不把聊天配置错误误报成连接失败。

产品选择 collection、Workflow 和 Workspace 后，Session 在必要时刷新 catalog，精确 get Workspace，只有 Not Found 才创建同一个名字。创建结果不确定时仍精确 get 同一名字 进行确认。校验 Profile 版本及 Workflow 归属后，通过 reload-with-options 准备目标； 只有返回 RUNNING 且 active name 匹配才发布新的 current Workspace。版本不一致允许 刷新 catalog 后再尝试一次，所有步骤共享同一个单调时间总期限。

`target_workspace` 在准备开始时更新；`current_workspace` 只在服务端确认后更新。 切换失败保留旧名字用于展示，但 workspace phase 为 FAILED，不能假定旧目标仍可对话。 相同有效 Workspace 和参数可复用就绪结果；复用前仍校验所选 Workflow 的 collection 归属，不在每轮对话重复 reload。

Conversation 创建在同一个准备操作中完成 Workspace 校验，然后绑定当前 Workspace。后续切换成功时，核心在旧 generation 结束后更新保留 route 的目标，下一次输入使用新 Workspace。 产品显式调用 Session audio start/end 开启或结束输入；回复、取消和完成沿用现有 Conversation callback。Session 先更新自身状态，再转发 callback。产品必须使用 Session 对应的 release 释放该 route，不能在活动对话结束前释放。等待或回复期间调用 audio start 由核心取消旧 generation，等待取消分发后在同一路由开始新输入；不要求产品先判断 UI 状态。重复 start（输入已开）与重复 end（输入已关）幂等，空闲 end 不会复活旧轮次。

## 交互模式与对话状态

快照中的 `parameters` 复用现有 Workspace 参数类型，`input` 为 Push-to-Talk 或RealTime，`initiative` 保持既有 PEER / AGENT 含义；不引入新的首句发言者字段。只合并成功应用的 patch，省略成员保留原值，失败保留此前已确认参数。

| 交互模式 | 对话状态 |
| --- | --- |
| Push-to-Talk | IDLE → RECORDING → WAITING → IDLE |
| RealTime | IDLE ↔ CALLING |

PTT 松手时冻结本轮 PCM 输入窗口：窗口非空才进入 WAITING；窗口为空则回到 IDLE。输入结束发出后本轮请求即完成，不等待服务器回复。WAITING 只是本地状态：按下后到下一个下行音频 BOS 之前的音频被丢弃（见 Audio 文档的 `waiting_for_bos`），一旦有之后的下行音频写入 Track 就回到 IDLE，由声音接管；`H2_GIZCLAW_SESSION_WAIT_MS`（5 秒）内没有任何下行音频也静默回到 IDLE，不报错。该判断在读取快照时进行。没有“回复中”状态。按下前和松手后的 PCM 不计入本轮，按住时长不是判据。

PTT 在录音期间收到下行音频也不能关闭输入或影响松手 EOS。RealTime 的文本和音频都不改变 CALLING；必要的轮次重启由 Session 处理。停止、取消或失败回 IDLE，错误保存在结果字段。Registration/catalog/workspace 的准备状态独立于这组对话状态，产品不能将它们拼成另一套控制音频的状态机。

Workspace 切换或 reload 先关闭旧输入并取消旧 generation，丢弃旧播放、发布 IDLE，只等待本地取消分发，不等待 Agent 回复完成。期间旧事件不转发到产品。RPC 成功且目标确认为 RUNNING 后发布有效参数和 Workspace；失败时Workspace 标为 FAILED，保留旧名称和参数作为显示信息。调用在控制任务执行，`service_poll` 必须持续运行以分发取消完成；同一 Session 的 Workspace RPC 串行。

同步 delete 只在名字等于 `current_workspace` 时参与 Session：与切换相同，先关闭旧输入、取消旧 generation 并占用同一个串行 Workspace RPC 槽位，已有 Workspace RPC 进行中时返回 BUSY。删除成功后 workspace phase 回到 EMPTY，清空 `current_workspace`、`workflow_name` 和已确认的 `parameters`，Conversation route 仍由产品通过 Session release 释放；下一次 select 或 Conversation 创建按正常准备流程 get、Not Found 时创建同名 Workspace 并 reload。删除失败或结果不确定（例如超时）时 workspace phase 为 FAILED，下一次 select 重新 get 校验，不把可能已被删除的名字当作就绪。删除其它 Workspace 不改变 Session 状态，也不打断对话；Session 关闭后 delete 与其它 Workspace RPC 一样返回 CLOSED。
## 对话错误详情

Conversation 的远端 ERROR 只表示服务端拒绝了本轮输入，在事件、完成回调和 Session 快照中保留原始 `error_code` 与 `retryable`；下行流的结束（有无错误码）都不是 ERROR。完成结果拥有错误码副本，释放本轮请求后仍可在完成回调中读取；Session 在转发错误事件前更新快照，并在完成后保留详情。产品展示错误时读取这些字段和 `error_stage`，不能只用通用 `last_error` 显示 `STREAM ERROR`。PAL 完成状态仍表示通用失败，不替代服务端错误原因；服务端只提供笼统错误码时，客户端不会推测更具体原因。

收到远端错误时，Service 日志输出 `remote_error code=... retryable=...`。新一轮输入成功启动时清除旧错误；新的准备操作完成或 Conversation 创建结果也会替换最近错误状态，没有远端详情时错误码为空且 `retryable=false`。`retryable` 透传服务端提示，不触发自动重试或改变 catalog、Workspace 的有效性判定。测试覆盖输入就绪前的拒绝、请求释放后的详情读取，以及 Session 快照副本和下一轮清除行为。

## 并发与生命周期

Session 借用 Service、PAL 和配置中的 collection 字符串。准备操作在调用方的后台任务 执行，不能从 `service_poll` callback 或 Service 网络任务调用。一个准备操作拥有网络 编排；select/conversation 在总期限内等待先前准备，register/refresh 遇忙返回 BUSY。 读取只短暂锁定状态或有界 catalog，不执行网络 I/O，也不暴露可变内部指针。

同步 Workspace RPC 在 Service mutex 下取得 Session 引用，并在 RPC 完成后释放。destroy 遇到尚未进入或尚未退出的 RPC 返回 BUSY；引用清空后先 detach，再销毁 Session 同步对象，避免 RPC 与销毁竞争访问已释放状态。

状态 revision 的变化会 notify 可选 Runtime；通知可合并，消费者醒来后重新读取。 这是状态通知，不是要求每个变化都恰好投递一次的事件队列。页面不拥有 Session 生命周期。

取消准备使 operation generation 失效并唤醒等待者；已经发出的 RPC 仍受剩余期限限制， 返回后不再开始下一步或提交结果。取消不回滚服务端已经执行的副作用；后续选择必须重新 确认。关闭 Session 永久拒绝新操作并丢弃迟到结果。Service 断开时 integration 调用 Session close；显式退出先 close，再 stop Service 以中断网络，dispatch drain 并 join 调用方任务，释放 Conversation，最后 destroy Session 和 deinit Service。重连新建两者。

## 验证

Session 测试在 typed RPC 边界注入结果，执行真实的库内状态管理。覆盖自动分页加载、 读取副本隔离、混合版本拒绝、空页循环限制、总超时、等待中的选择、取消等待、创建响应 丢失后的精确恢复、重复选择复用、切换失败、Conversation 回调释放、删除当前 Workspace 后先取消对话再回到 EMPTY 并按 get、create、reload 重新准备、删除失败置 FAILED 后重新校验、删除其它 Workspace 不影响就绪，以及关闭后迟到 catalog/activation 不提交。真实服务器和设备验收与这些自动测试分别记录。Portable E2E 的普通 Voice case 从连接注册开始使用 Session，并在 PTT、Realtime 和 route 释放边界验证公共快照；AMOLED 可通过 `H2_GIZCLAW_E2E_VOICE_ONLY` 单独运行该流程，见 [AMOLED Session E2E](/apps/h2loader/boards/amoled/gizclaw_e2e)。

## 自动系统校时

Service 在首次连接前读取 Time PAL。时间无效或为 0 时，由网络任务先向同一 `server_endpoint` 请求 `GET /server-info`，校时成功后才发起带时间戳的信令连接；失败保持可取消的 30 秒重试，stop 会取消 HTTP 和唤醒等待。已有有效时间时可直接连接，连接成功后启动独立的 `$gizclaw/time` 任务刷新时间，其他通信不等待该 HTTP 请求。首次连接前已成功校时的同一 Service 不重复启动校时任务。请求超时为 5 秒，失败后按单调时间等待 30 秒重试，直到成功或 Service 停止。停止取消在途 HTTP 并等待任务退出。Service 是单次连接生命周期：断线进入 terminal，停止后的实例不能再次 start；重连须新建 Service，新实例按当前时间有效性选择连接前校时或连接后刷新。已完成校时任务的句柄由 stop 回收；普通网络 poll 不重复校时。校时任务创建失败也按 30 秒重试，不触发连接 terminal。

`h2_gizclaw_service_get_time_sync_status()` 返回 WAITING、RUNNING、RETRY 或 SUCCEEDED，以及最近结果和 HTTP 尝试次数；该状态描述本次校准，不代表时钟 是否有效。失败保留此前有效系统时间。响应必须是合法 JSON，顶层 `server_time` 必须为正整数毫秒时间戳，使用十进制整数字面量，不能是字符串、负数、零、分数或指数形式。

Time PAL 的 `h2_pal_time_get_wall_ms()` 是业务读取时间的公共入口： `get_wall_status().valid == false` 返回 `H2_PAL_TIME_ERR_UNCALIBRATED`，状态查询 或时钟读取错误则保留原错误，失败输出清零。先检查状态，`valid == false` 时不调用底层读取方法；不支持 wall clock 的 provider 返回 UNSUPPORTED。BK3633 的无效 RTC 日历表示未校准，RTC 传输失败及 Web 宿主时钟不可用仍保留原错误。`get_wall_ms()` 成功表示已通过有效性检查；没有额外的有效时间读取方法。超时、重试及耗时始终使用 monotonic API。

时间值始终是 UTC Unix 毫秒，不添加时区偏移。产品显示层负责时区；H106 使用 北京时间 UTC+08:00。未校准时不得把自启动以来的计数显示为当前时间。

ESP32/BK7258 的校准状态由读回的时间自身判定，不保存任何标记：`set_wall_ms` 本来就把时间写进 RTC 支撑的系统时钟，RTC 在深睡眠、软件重启和自重启期间继续计时，因此读数不早于 2020-01-01 就认为仍带着上一次校准，`valid` 为真、来源为 RTC；上电和掉电复位会让 RTC 从纪元附近重新开始，读数落后即判为未校准，等待下一次 `set_wall_ms`。读时钟失败按错误上报，纪元前或非法的 `timeval` 在转换成无符号前被拒绝，不会绕过门限。这样充电唤醒和自重启后 UI 仍能显示时间。深睡眠期间由 RTC 慢时钟维持，可能有分钟级漂移，连接后由 GizClaw 校时纠正。BK7258 CP 核没有 RTC 访问，`get_wall_status` 仍恒为无效。保留运行上下文与系统时钟的轻睡眠保持有效。更换或丢失时钟的 provider 必须清除 valid。桌面/Web provider 可依据宿主有效的系统时钟提供有效 UTC。`set_wall_ms` 是显式外部设置， ESP32/BK7258 标记 USER 来源；它不声称执行过 NTP 协议。

自动测试覆盖无效时间时校时先于连接、已有有效时间时连接前无请求、未校准读数、失败后重试、非法响应、校时中通信继续、 校时后 UTC 读数、断开后保留有效时间、重建 Service 后再次校时、成功后跨重试期限的普通轮询不重复校时、校时请求中和重试等待中停止、设置失败保留旧值及模拟重启失效。 ESP Time provider 的 host 测试（`time_retention_test`）覆盖纪元附近读数无效、设置后有效且来源为 RTC、跨重启携带的时钟仍有效、刚好早于门限的读数无效、时钟丢失后失效、被拒绝的设置不改变时钟、读失败上报错误及纪元前读数不越过门限；真实硬件的轻睡眠/深睡眠保留行为仍需分别做设备验收。

通过 `runtime->time` 调用 `h2_pal_time_set_wall_ms()` 成功后，Runtime 通过包装既有 Time vtable 的 `set_wall_ms` 自动发布 `H2_RUNTIME_SYSTEM_EVENT_TIME_ADJUSTED`，组件为 `H2_RUNTIME_COMPONENT_SYSTEM_TIME`，payload 为 `h2_runtime_system_event_time_adjusted_t`（请求设置的 UTC `wall_ms`）。GizClaw 和其他应用调用者共享该行为；失败不发事件，读时钟和 sleep 不发事件。事件 envelope 的时间戳仍为单调时间。必须把 Runtime 的 Time PAL 传给 Service；直接调用底层 provider 会绕过 Runtime。事件遵循现有有界队列的溢出丢弃规则，不改变已经成功的设置返回值；消费者收到事件后重读有效时间，并保留周期刷新作为溢出恢复。并发设置的事件是刷新提示，不应将 payload 当作当前时钟快照。

## 通用资源状态

Contact、个人资料、FriendGroup、AppConfig 和 Firmware 使用独立的 `h2_gizclaw_resource_t` 实例。每个实例只拥有一种资源，按需创建并借用 Service、PAL 和可选 Runtime；与 Conversation Session 分开分配和串行化，不让联系人加载阻塞 Workspace 的准备锁。同一 Service 上同种资源的 mutation 应统一经过它的 Resource。

Resource 在库内保存有界快照，公开读取深拷贝到调用方 storage。`valid` 区分有效空列表与未加载；`stale` 标记正在刷新、失败或断开后不能保证新鲜的数据；`busy`、`closed` 和 `last_error` 描述操作状态。`revision` 用于通知重新读取，`data_revision` 用于识别新提交的数据；两者都是本地计数，不能解释为服务端 revision。失败保留旧快照，关闭后仍可读到标记 stale 的旧数据。

Contacts 和 Groups 刷新完整拉取有界列表，拒绝重复 identity、无效 cursor、超出容量和无限分页。联系人创建使用调用方提供的稳定 name，只有 Not Found 才创建，响应不确定时 get 同名资源并校验；修改和删除成功后重新加载完整列表。Profile 更新后 get 完整资料再提交，避免由页面合并两份字段。

AppConfig 对应 GizClaw 0.16.3 的只读 `server.app_config.list/get`，读取当前选中 RuntimeProfile 的配置。底层 `h2_gizclaw_req_create_app_config_list/get`、`h2_gizclaw_resp_parse_app_config_list/get` 与同步 `h2_gizclaw_rpc_app_config_list/get` 沿用统一请求和调用方 response storage。键遵循 1–63 字节、点分隔的小写 kebab-case；值最多 4096 字节，按 `value.data/len` 原样返回，空字符串与 Not Found 不同。库不解析 JSON，也不提供客户端写接口。

使用 `H2_GIZCLAW_RESOURCE_APP_CONFIG` 创建 Resource，执行 `H2_GIZCLAW_RESOURCE_REFRESH` 后，从 `snapshot.data.app_config` 读取完整 `items[key, value]` 和服务端 `runtime_profile_name/revision`。刷新遍历所有分页并逐项 get，所有响应必须具有同一 Profile name/revision；版本变化、键消失、容量不足、期限耗尽或关闭均保留旧快照并标记 stale。成功刷新会整体替换快照，因此删除的键会消失，空配置也可以是 valid。`max_items`、`page_size`、`storage_bytes` 由消费者给定；storage 需容纳旧快照副本、分页结果和新值。该状态仅保存在内存，产品自行决定何时刷新、解释配置及是否持久化；不会自动订阅配置变化。

执行发生在调用方 worker，不能从 Service worker 或 poll callback 调用。每次 execute 的 timeout 是所有 RPC 的单调时间总期限；同一 Resource 并发 execute 返回 BUSY。close 永久关闭 admission 并丢弃迟到结果，调用方先 close、stop Service，再 join worker 和 destroy Resource；重连新建实例。页面取消仅丢弃页面结果，不回滚已执行的服务器 mutation，资源快照可以继续更新。

Resource 不保存 UI 草稿、页面焦点、产品默认值、电话号码放行策略或文件路径，也不会自动把网络快照持久化为离线授权。例如 H106 只有在完整 Contact 快照通过产品校验且持久化成功后才替换离线通话白名单；网络连接消失不会删除这份已提交的产品白名单。磁盘读取和写入失败仍由该持久化边界报告。

Resource 的 `h2_gizclaw_resource_test` 直接执行生产 Resource，仅在 typed RPC 边界提供替身；覆盖完整分页、重复/循环游标、容量不足、快照复制、创建响应丢失恢复、期限/关闭交错。E2E consumer 故障测试和真机验收单独记录。

## Firmware 元数据状态

使用 `H2_GIZCLAW_RESOURCE_FIRMWARE` 创建 Resource，并设置正数 `firmware_channel`；每个实例固定一个 channel，允许未来的正数 channel。该资源不使用 `max_items/page_size/storage_bytes`，它们可为零。调用方 worker 显式执行 `H2_GIZCLAW_RESOURCE_REFRESH`，通过 `snapshot.data.firmware` 读取 channel、description、URL、SHA-256、size，以及 GizClaw 0.16.5 新增的 `has_version/version`。version 是最多 128 字节的包 SemVer；缺省时 `has_version=false` 且字符串为空，不能据此推断当前设备版本。

快照沿用 `valid/stale/busy/closed/last_error` 和 revision 通知。刷新失败、超时、返回 channel 不匹配或关闭后迟到结果均不覆盖旧快照；成功刷新会替换全部元数据，包括清除旧版本号。读取复制整个内联结构，不占用 response storage 字节，可传入清零的 storage。这里只读取并缓存服务器固件元数据，不自动轮询、比较版本、下载或安装。

### 内部诊断与 Session 快照

PTT 的首个音频 worker 失败阶段和首次取消来源仅写入内部诊断日志，不新增或改写
Session 快照字段。`error_stage`、`last_error`、远端错误详情的保留规则，以及
generation、取消完成和 callback 的生命周期保持本页既有合同。日志中的 request
generation 用于关联本轮执行，不能替代 Session generation 或产品自己的轮次标识。
