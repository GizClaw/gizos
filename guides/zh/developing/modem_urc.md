# Modem URC 接收与并发合同

公共 owner 是 `libs/pal/providers/modem/common` 和 `quectel`；板级 UART、CMUX、PPP 生命周期仍由 consumer 持有。队列容量保持 16，不以扩容替代过滤或并发控制。

## 接收

`h2_modem_rx_feed` 持有有界尾片段，只在 CR/LF 后交付完整行。物理流绝对 offset 区分新字节与累计前缀，不比较字符串进行去重。重复的 RING 是独立 occurrence；重放的相同 offset 不是。超长行或 NUL 污染行丢弃至下一个分隔符，不把截断尾巴当作新行。返回首个错误但继续消费当前输入，不能通过重放整个 batch 重试 FULL。

`h2_quectel_rx_feed` 在入队前分类。普通 OK、echo、IMEI/IMSI 等不进入 URC worker。注册查询的 `<n>,<stat>` 与通知的 `<stat>[,"lac",...]` 按语法区分，即使查询期间也保留真实通知。CPIN、CGATT、CSQ、CLCC、QSIMSTAT 在对应查询期间存在同格式歧义，必须提供命令上下文；确定为独立通知的行可由 transport 直接调用 `h2_quectel_post_urc_line`。不支持凭内容推断两次同文本是同一 occurrence。对于 `AT+CPIN?`，RX tap 对精确匹配的 `+CME ERROR: 10` 和 `+CME ERROR: SIM not inserted` 只原子写入本次 CPIN 的缺卡标记，不获取 state lock，也不投递 URC worker。共用 AT exchange 入口在每次 CPIN 交换前清除标记；失败返回后，在 reset/SIM generation 未变化时持 state lock 调用 `h2_quectel_sim_update` 记录 ABSENT，使同一次 `get_status()` 返回 ABSENT，并按状态边沿发布 `MODEM_SIM_CHANGED`。这避免 command transport 丢弃错误文本后漏报 SIM 缺失，不改变同步 AT 响应的 URC 分类与帧边界；后续成功的 `+CPIN: READY` 仍恢复 READY。

receiver 为每个 AT 通道单独配置，生产者串行调用，不能输入 PPP 数据通道。命令上下文必须覆盖完整行，不得在尾片段中途切换。停止并 join 所有生产者之后才能重置 receiver 或销毁 modem。

## esp_modem 1.4.3 consumer 接线

`DTE::command_cb::process_line` 的旧 URC hook 在命令 parser 之前调用，参数为 `data, consumed + len`，包括普通应答。非 CMUX 使用累计 buffer；CMUX 开启 inflatable 时也可能累计，关闭时则直接提供片段。旧 hook 没有 consumed、buffer epoch 或物理 offset，不能仅靠长度、指针或相同前缀无歧义恢复物理流。

consumer 必须在只交付新字节的边界调用 `h2_quectel_rx_feed`，物理 offset 每次增加实际读取长度，零长度输入不推进 offset。命令 parser 仍取得原始完整输入，不能被 framer 消费或修改。

- 非 CMUX：观察 UART 终端实际读出的字节。Firmwares 以 `-Wl,--wrap=uart_read_bytes` 包装读取，只处理 modem UART 端口；这正是 DTE 追加到累计 buffer 的字节，因此不受 buffer 重置、command 成功/超时影响。
- CMUX（未开启 inflatable）：旧 hook 收到的就是 AT 通道的单个 payload 片段，可以直接作为来源；UART 字节此时是帧，不能再输入 framer。开启 inflatable 后 hook 又会累计，consumer 必须拒绝该配置。
- 切换来源或 CMUX 进出数据模式（AT 通道换 DLCI）时重置 receiver；切到 CMUX 前先切来源，最多重复框出 `AT+CMUX` 的回显/OK，不会把帧当文本。

不能修改共享 SDK/cache。私有 `tiga_modem_urc_line(data,total_len)` 的切行循环必须删除。transport 在命令发布、完成时同步 RX 命令上下文。禁止同时从 command response 再交付已经由 RX worker 负责的真实 URC。启用异步 worker 后，公共 command-response 路径假定 transport 负责全部真实 URC；同步 read/write 模式由公共 AT parser 负责。

## 锁和生命周期

锁顺序为 operation lock → state lock。operation lock 保持完整公共操作和 AT 事务串行；嵌套操作只保留一次 state lock acquisition。阻塞 command/read/write 期间释放 state lock，保留 operation lock。URC 行处理只取 state lock，所以不等待 identity 或注册查询的 AT timeout。Quectel 的后台识卡使用同一 worker 的 idle maintenance；它先 try operation lock，忙则让出，绝不阻塞等待其他 AT 操作。启用 Quectel worker 的 sync provider 必须实现 try_lock_mutex。纯缓存 getter 同样只取 state lock。

URC 的系统事件、SIM invalidation 和 sleep gate 回调不得重入 modem API，不能等待 AT/RX/URC 任务。state lock 的短临界区不意味着任意 consumer 回调都自动无阻塞，板级实现必须遵守合同。close 的 transport teardown 在 state lock 外执行，deinit 在生产者停止后于所有 provider lock 外 join worker。join 失败保留实例供重试。SIM/reset generation 用于拒绝跨失效边界的 command response。

### URC 任务栈预算

`$modem/urc` 不仅解析通知，还会在空闲回调中执行 AT 交换（来电 CLCC 看门狗、SIM 就绪轮询和后续刷新）。集成方配置任务栈时，必须覆盖 worker/provider 的调用链峰值，加上一次完整 `command` 回调及其 AT 缓冲、串口/CMUX 驱动等下层调用的栈开销，并留安全余量；还需覆盖事件和 sleep gate 回调路径。建议以 **至少 8192 字节**作为起始预算，若上述峰值与余量之和更大则继续增加。这不是所有 transport 的充分保证；4096 字节即使放在 PSRAM 中也不能视为通用安全值。注意平台任务 API 的栈单位可能是字节或 word，必须正确换算，并在目标板来电、SIM 热插拔及超时场景测量栈高水位。

provider 将空闲维护的解析响应和共用 AT 交换的原始 command 响应分别保存在实例中，避免两块约 768 字节响应叠加占用任务栈。维护缓冲只在 `operation_begin` 到 `operation_end` 内使用；operation lock 跨越 AT 等待，释放 state lock 不释放缓冲所有权。其它命令路径和 DSCI/URC 行处理不使用维护缓冲，原始响应与解析响应也不重叠；无 sync provider 时由调用方串行化。回调禁止重入 AT 操作。DSCI 通知处理不执行 AT，也不分配大响应对象。

## 事件与诊断

注册、packet、signal 按语义字段变化发布，保留 A → B → A；call/READY 不作全局去重。close/reset 清除观察状态。系统事件采用 timeout 0，失败累计 event_drop_count；观察状态不等价于 Runtime 已收到事件。

`h2_modem_urc_get_stats` 提供 accepted、handled、full、truncated 计数，不输出行文本、身份或 secret。并发快照各字段独立读取，不是事务一致的队列长度。FULL 证明此队列入队失败，不能据此证明 Runtime event queue 满或组合键失效。

## 验证边界

公共回归包括分片/累计重放、重复真实通知、普通应答风暴、长事务期间消费者进度、状态边沿和生命周期失败重试。固件构建及真实按键、注册、SIM、PPP 验收由 consumer 在配对接线后完成；host 测试不能代替设备结论。

## 插卡后的就绪恢复

`h2_quectel_is_urc` 接受 `+QSIMSTAT` 和非对应查询期间的 `+CPIN:`；
`h2_quectel_handle_urc_locked` 将 READY、SIM PIN/SIM PUK 分别映射为 READY、LOCKED。
对应 `AT+CPIN?` 的响应由 AT parser 同步处理，不依赖入队。`+QIND: SMS DONE`、`+QIND: PB DONE` 和 `Call Ready` 进入队列，仅合并触发一次主动查询，不直接作为 READY 依据。

新插入边沿先发布 UNKNOWN，只安排后台工作，回调不执行 AT。
worker 在队列空闲 1000 ms 后执行一次维护，每轮最多一个 1000 ms 超时的 AT 交换，
每条通知之间仍先处理队列；持续通知或 operation lock 忙会推迟维护，30 次是尝试上限，
不是 30 秒墙钟承诺。先最多尝试 30 次 `AT+CPIN?`，取得 READY 后依次执行
`AT+CIMI`、`AT+CEREG?`、`AT+CGATT?`，用剩余预算重查注册/附着直至恢复。
IMSI 仍由既有 get_identity 现场读取，不引入身份缓存或记录 IMSI 日志。
SIM、注册、packet 沿用系统事件和语义去重；LOCKED 停止轮询，后续 READY URC 可启动刷新。
拔卡、reset、close 取消待办；重复插入通知不重置预算，真正拔出后再插入得到新预算。
缺卡后的 CPIN READY 是独立插入证据，可恢复 READY；缺卡期间另以约 5 秒空闲间隔持续执行单次有界 CPIN 探测。没有 worker 的同步使用者继续通过 get_status 查询。

移远[官方热插拔 FAQ](https://www.quectel.com/faqs/10-1-how-to-enable-hot-swap-function-of-sim-card/)
列出 `+CPIN: NOT READY`、`+QSIMSTAT: 1,0/1,1` 及 Call Ready，
但不是 H106 Zero EC800M 特定固件的实测序列。本次只有台架现象描述，没有原始 UART1
捕获，不能证明该固件会发 READY 或某个 QIND，也不能证明其必须执行 CFUN。
因此恢复不自动执行 CFUN；仅保留 QSIMDET 配置改变时既有的一次 restart_module 流程。
若 30 次 CPIN 后仍未就绪，应采集 SIM_DET 电平、QSIMDET/QSIMSTAT 读回、
CPIN 应答及完整通知顺序，再依据该固件的移远说明决定是否需要 SIM 重新初始化。

## 语音呼叫状态通知

Quectel provider 在每轮实际 prepare 中，与 CLIP 等通知配置一起 best-effort 下发 `AT^DSCI=1`。该设置不保存，模组重启后需要重发；明确 `ERROR` 表示固件不支持，在当前 provider instance 内记住且不重试，不影响 prepare 成功。超时等 transport 故障不锁存为不支持，下轮 prepare 仍尝试。重复调用已完成的 prepare 不重复配置。

`^DSCI: <id>,<dir>,<stat>,<type>,<number>,<num_type>` 始终按 URC 分类，包括 AT 命令等待期间。只处理 type=0 的语音通知，type=1 的 PS 通知不改变呼叫状态或活动保持。状态 1/2/3/4/5/6/7 分别映射 HELD/DIALING/ACTIVE/INCOMING/WAITING/ENDED/ALERTING。MT INCOMING 与 RING/CLIP 共享 provider 的来电 ID；CLIP 可补充号码，CONNECT 发布状态变化，CALL_END 立即发布 MODEM_CALL_ENDED 并释放来电 ID 和活动保持。

观察到有效语音 DSCI 后，以有呼叫 ID 的 DSCI 作为远端结束依据，直到模组 reset；无 ID 的 NO CARRIER/BUSY/NO ANSWER 仍可作为 AT 结果，但不再发布呼叫结束，以免迟到通知误结束下一通。未观察到语音 DSCI 时保留原有终止 URC 回退。DSCI、CLCC 和本地接听/挂断结果共享事件状态去重；未启用 DSCI 路径的重复响铃通知保持原合同。AT 等待期间发生的新呼叫状态优先于旧的命令结果。当前 PAL 仍是单呼叫快照，不增加多通并发呼叫管理；同一模组 ID 的复用依赖有序 URC。

Host 测试覆盖支持/不支持/超时的 prepare、来电主叫挂断立即结束、迟到 NO CARRIER、连续来电、接听/挂断、CLCC 去重和 PS 忽略。实际固件是否上报 DSCI、通知时延及串口接收完整性仍需台架验收。

### 来电响铃看门狗

配置 URC task/queue API 后，provider 复用 `$modem/urc` worker 的有界空闲等待，在未接听的 MT INCOMING/WAITING 期间查询 `AT+CLCC`。RING、初始 CLIP 或 CLCC 均可建立来电；结束后迟到的 CLIP/CLCC 仍忽略，下一次 RING/DSCI 或主动查询可建立新来电；本次来电收到有效语音 DSCI 后停止轮询，上一通的 DSCI 不会禁用下一通的兜底。默认空闲周期为 `H2_QUECTEL_RING_POLL_INTERVAL_MS=1000`，可通过编译常量调整（同时影响该 worker 的 SIM recovery 空闲周期）。持续 URC 流量、其它 AT 操作或 transport 延迟可推迟检测，不承诺硬实时一秒结束。

看门狗先 try-lock operation mutex，忙则跳过本轮；持锁后重新检查来电与 opened 状态，保留已有 state-lock 释放/恢复和睡眠 gate 合同。单次查询使用 `H2_QUECTEL_RING_POLL_TIMEOUT_MS=1000` 毫秒预算；command transport 必须遵守传入超时，原始 read/write 路径按总等待预算限制 I/O。失败、超时、截断或歧义列表视为未知，不打印周期错误、不发布结束，下轮重试。成功列表按已知模组 ID（尚无 ID 时按 MT 和可用号码）关联；来电消失即复用终止事件去重发布 MODEM_CALL_ENDED，迟到 NO CARRIER 不重复。ACTIVE 发布状态变化并停止；本地接听、拒接、结束及 close/deinit 同样停止，迟到 RING 不重新开启已接听来电的看门狗。查询期间收到的新状态优先于查询结果。

该能力仅覆盖未接听来电，保持现有单呼叫快照，不监控已接通通话；未配置 worker 的同步使用方式没有自动轮询。Host 测试验证周期回调、超时未知、迟到终止去重、DSCI 按次禁用、CLCC 接通和本地生命周期停止；串口实际结束时延仍需硬件验收。

## 首次 open 与 SIM 启动通知

`open` 打开 AT/控制通道，不等待 SIM READY 或网络注册。初始化 SIM 为 UNKNOWN、
查询发现 ABSENT、prepare 期间插卡再 READY，都不应仅因 SIM generation 改变而失败。
物理拔卡仍立即使数据失效，ABSENT/LOCKED 仍阻止 PPP；物理缺卡之后的迟到 CPIN READY
仍被忽略，必须先收到插卡状态。READY 本身不增加 SIM generation。

`h2_quectel_modem_open` 的错误传播路径如下：

- `operation_begin` 获取 operation/state 锁；随后调用 board `init`，原样传播其错误。
- `prepare` 设置 `preparing=1`，依次执行 AT、ATE0、CMEE、CLIP、可选 DSCI、
  CREG/CGREG/CEREG、两个 RI QCFG，再执行 power prepare 的 CGMM、QSIMDET 查询、
  QSIMSTAT 通知使能和可选 QSCLK。AT、CLIP 和 power prepare 的错误直接传播；
  其余为 best-effort，但整轮最终仍检查 reset generation。
- 两种 AT transport 都在交换期间检测 reset generation；只有非 prepare 交换才因
  SIM generation 变化返回 `H2_PAL_ERR_INVALID_STATE`。该保护已由提交
  `02baf05235722892fe716e535a3e9c895aafa08c` 引入。
- QSIMDET 已匹配不会设置 `sim_restart_required`。只有配置不匹配并成功写入后才要求
  重启；有回调时最多重启一次并重新 prepare，无回调、重启后仍不匹配或已有待重启
  标志时保留错误。意外 RDY/reset 仍取消当前 prepare，不能当作普通 SIM 边沿忽略。
- prepare 结束清除 `preparing`；失败时调用 board `deinit`，成功时设置 `opened`。
  `operation_end` 只处理锁和 sleep reconcile，没有额外的 SIM generation 取消判断。
  board command/sleep gate/锁回调返回的错误同样可以透传为 -7。

回归测试在 14 个 prepare AT 步骤分别注入 ABSENT→READY、ABSENT→插卡 UNKNOWN→READY、
真实拔卡和 RDY，覆盖 command 与 read/write 两种 transport，共 112 个组合。
移除 prepare 的 SIM generation 豁免后，新增 open 成功断言失败；保留修复则全部通过。
这些测试不证明台架固件包含该提交，也不覆盖外部 board 的 CMUX 恢复实现。

若集成固件仍报首次 open=-7，应记录实际 GizOS revision、board init 返回码、失败 AT
命令与返回码、交换前后 reset/SIM generation、preparing 和两个 sim_restart 标志。
只有 `H2_MODEM_RECOVERY stage=awake rc=0` 与稍后的应用 SIM 事件，无法区分 board init
后续失败、transport 返回 -7、意外 RDY 或旧版本的 SIM generation 误取消；应用事件
消费时间也不能确定通知落在哪条 prepare 命令内。

## 板级电源关闭与重新打开

覆盖 open/close 的 board 在停止 RX producer、释放 transport 并完成下电后，持 operation lock 调用 provider 的 transport-closed 通知；它清除 SIM presence、去重与注册观察，保留 worker 与回调生命周期。每轮 prepare 使能 QSIMSTAT 后主动查询 QSIMSTAT/CPIN，不依赖可能早于 UART 路由配置的开机通知；暂未 READY 的插卡状态启动有界轮询。缺卡后收到 CPIN READY（包括主动查询应答）即恢复插入与 READY，发布 SIM 事件并刷新 IMSI、注册及附着状态，不再等待 QSIMSTAT 插入通知。

ABSENT 期间，已有 URC worker 每约 5 秒空闲时间执行一次 AT+CPIN?，单次超时 1 秒，使用实例 response 缓冲与 operation try-lock，不在 RX 回调执行 AT；关闭实例或板级 transport 后停止。ERROR / NOT INSERTED 不产生 READY。`+QIND: SMS DONE`、`+QIND: PB DONE`、`Call Ready` 仅合并触发一次 CPIN 查询，每次拔卡重新允许触发。

### 操作锁与恢复事件

板级可成对提供 operation lock/unlock 回调，通过 transport_user 复用递归操作锁及 holder 诊断；普通操作请求 15000 ms，后台维护使用零超时 try-lock，竞争时让出。operation_allowed 在锁前及锁后检查取消或初始化占用，锁后拒绝会释放本次获取；板级必须允许负责 close/deinit 的任务执行清理。回调生命周期覆盖实例，不能重入 modem 操作。未配置回调时保留 PAL mutex 行为，非零 timeout 仍使用传统阻塞获取。

重启前只失效旧状态，不发布 MODEM_READY；真实 RDY/APP RDY 和完成 prepare 才发布就绪。每次新来电清除旧 DSCI 关联，只有本次来电已关联的 DSCI 状态才能由对应结束通知终止。

看门狗间隔与查询预算是 provider 级编译期覆盖参数，默认均为 1000 ms，支持 1–60000 ms，库和消费者必须使用一致定义，不提供实例运行期修改。间隔同时驱动 SIM 恢复空闲维护，close 停止操作、deinit 停止 worker；SIM 恢复单次 AT 预算仍为 1000 ms。
