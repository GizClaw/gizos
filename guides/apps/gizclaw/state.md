# GizClaw 状态与请求

`libs/gizclaw` 的 Session 持有连接作用域内的 Runtime Profile 身份、Workflow catalog、 Workspace 准备状态和 Conversation 状态。产品读取公共快照并投影到页面，不再维护另一份 可用于业务决策的 Profile/catalog/Workspace 状态。低层 Service/RPC 仍可独立使用； 选择 Session 的同一 Service 必须统一通过 Session 执行注册、catalog、Workspace 修改和 Conversation 操作，不能混用原始接口绕过状态所有者。

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

Conversation 创建在同一个准备操作中完成 Workspace 校验，然后绑定固定 Workspace。 产品显式调用 Session audio start/end 开启或结束输入；回复、取消和完成沿用现有 Conversation callback。Session 先更新自身状态，再转发 callback。产品必须使用 Session 对应的 release 释放该 route，不能在活动对话结束前释放。新一轮输入必须等待前一轮 completion 已分发；没有活动 generation 时 end 返回 INVALID_STATE，不能把终态改回 ACTIVE。

## 并发与生命周期

Session 借用 Service、PAL 和配置中的 collection 字符串。准备操作在调用方的后台任务 执行，不能从 `service_poll` callback 或 Service 网络任务调用。一个准备操作拥有网络 编排；select/conversation 在总期限内等待先前准备，register/refresh 遇忙返回 BUSY。 读取只短暂锁定状态或有界 catalog，不执行网络 I/O，也不暴露可变内部指针。

状态 revision 的变化会 notify 可选 Runtime；通知可合并，消费者醒来后重新读取。 这是状态通知，不是要求每个变化都恰好投递一次的事件队列。页面不拥有 Session 生命周期。

取消准备使 operation generation 失效并唤醒等待者；已经发出的 RPC 仍受剩余期限限制， 返回后不再开始下一步或提交结果。取消不回滚服务端已经执行的副作用；后续选择必须重新 确认。关闭 Session 永久拒绝新操作并丢弃迟到结果。Service 断开时 integration 调用 Session close；显式退出先 close，再 stop Service 以中断网络，dispatch drain 并 join 调用方任务，释放 Conversation，最后 destroy Session 和 deinit Service。重连新建两者。

## 验证

Session 测试在 typed RPC 边界注入结果，执行真实的库内状态管理。覆盖自动分页加载、 读取副本隔离、混合版本拒绝、空页循环限制、总超时、等待中的选择、取消等待、创建响应 丢失后的精确恢复、重复选择复用、切换失败、Conversation 回调释放，以及关闭后迟到 catalog/activation 不提交。真实服务器和设备验收与这些自动测试分别记录。

## 自动系统校时

Service 在首次连接前读取 Time PAL。时间无效或为 0 时，由网络任务先向同一 `server_endpoint` 请求 `GET /server-info`，校时成功后才发起带时间戳的信令连接；失败保持可取消的 30 秒重试，stop 会取消 HTTP 和唤醒等待。已有有效时间时可直接连接，连接成功后启动独立的 `$gizclaw/time` 任务刷新时间，其他通信不等待该 HTTP 请求。首次连接前已成功校时的同一 Service 不重复启动校时任务。请求超时为 5 秒，失败后按单调时间等待 30 秒重试，直到成功或 Service 停止。停止取消在途 HTTP 并等待任务退出。Service 是单次连接生命周期：断线进入 terminal，停止后的实例不能再次 start；重连须新建 Service，新实例按当前时间有效性选择连接前校时或连接后刷新。已完成校时任务的句柄由 stop 回收；普通网络 poll 不重复校时。校时任务创建失败也按 30 秒重试，不触发连接 terminal。

`h2_gizclaw_service_get_time_sync_status()` 返回 WAITING、RUNNING、RETRY 或 SUCCEEDED，以及最近结果和 HTTP 尝试次数；该状态描述本次校准，不代表时钟 是否有效。失败保留此前有效系统时间。响应必须是合法 JSON，顶层 `server_time` 必须为正整数毫秒时间戳，使用十进制整数字面量，不能是字符串、负数、零、分数或指数形式。

Time PAL 的 `h2_pal_time_get_wall_ms()` 是业务读取时间的公共入口： `get_wall_status().valid == false` 返回 `H2_PAL_TIME_ERR_UNCALIBRATED`，状态查询 或时钟读取错误则保留原错误，失败输出清零。先检查状态，`valid == false` 时不调用底层读取方法；不支持 wall clock 的 provider 返回 UNSUPPORTED。BK3633 的无效 RTC 日历表示未校准，RTC 传输失败及 Web 宿主时钟不可用仍保留原错误。`get_wall_ms()` 成功表示已通过有效性检查；没有额外的有效时间读取方法。超时、重试及耗时始终使用 monotonic API。

时间值始终是 UTC Unix 毫秒，不添加时区偏移。产品显示层负责时区；H106 使用 北京时间 UTC+08:00。未校准时不得把自启动以来的计数显示为当前时间。

ESP32/BK7258 冷启动和重置后的校准状态默认无效；即使 RTC 仍有读数也保守等待 重新校准。保留运行上下文与系统时钟的轻睡眠保持有效；导致程序重新初始化的 深睡眠唤醒按冷启动处理。更换或丢失时钟的 provider 必须清除 valid。桌面/Web provider 可依据宿主有效的系统时钟提供有效 UTC。`set_wall_ms` 是显式外部设置， ESP32/BK7258 标记 USER 来源；它不声称执行过 NTP 协议。

自动测试覆盖无效时间时校时先于连接、已有有效时间时连接前无请求、未校准读数、失败后重试、非法响应、校时中通信继续、 校时后 UTC 读数、断开后保留有效时间、重建 Service 后再次校时、成功后跨重试期限的普通轮询不重复校时、校时请求中和重试等待中停止、设置失败保留旧值及模拟重启失效。 真实硬件的轻睡眠/深睡眠保留行为仍需分别做设备验收。

通过 `runtime->time` 调用 `h2_pal_time_set_wall_ms()` 成功后，Runtime 通过包装既有 Time vtable 的 `set_wall_ms` 自动发布 `H2_RUNTIME_SYSTEM_EVENT_TIME_ADJUSTED`，组件为 `H2_RUNTIME_COMPONENT_SYSTEM_TIME`，payload 为 `h2_runtime_system_event_time_adjusted_t`（请求设置的 UTC `wall_ms`）。GizClaw 和其他应用调用者共享该行为；失败不发事件，读时钟和 sleep 不发事件。事件 envelope 的时间戳仍为单调时间。必须把 Runtime 的 Time PAL 传给 Service；直接调用底层 provider 会绕过 Runtime。事件遵循现有有界队列的溢出丢弃规则，不改变已经成功的设置返回值；消费者收到事件后重读有效时间，并保留周期刷新作为溢出恢复。并发设置的事件是刷新提示，不应将 payload 当作当前时钟快照。
