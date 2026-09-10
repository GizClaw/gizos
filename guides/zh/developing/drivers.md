# Drivers

`libs/drivers` 保存与具体 board 和 SDK 无关的芯片、传感器及外设 driver。Driver 负责设备协议和状态机，实际总线、GPIO、UART、sleep 和同步能力由调用方注入。

## API Reference

[API Reference](/references/drivers)

`libs/drivers` 各 driver 的 `include/` 目录中实际参与项目构建的头文件是 Drivers 的生产 Public API contract。

## Driver Families

### Quectel Modem

`modem/quectel` 实现 Quectel modem 的 AT command、URC、call、GNSS、cell locate、PPP 和状态处理，并输出 `h2_pal_modem_api_t`。Config 注入 transport callback、PAL sync、mem 和 system event API。

Quectel 和 SIMCom 共用 `providers/modem/common:urc`，通过 `h2_tasks` 声明 `$modem/urc`，每个异步 Modem 实例启动独立的 PAL task。接入方在 provider config 同时注入 `urc_task_api`、`urc_queue_api` 和 `sync_api`，并在 firmware target 配置该任务策略；`h2_tasks` 本身不启动任务。

串口接收回调调用 `h2_quectel_post_urc_line` / `h2_simcom_post_urc_line`，复制完整通知后立即返回。公共 worker 按 FIFO 顺序调用厂商解析器，获取 Modem 锁、更新状态和发布 PAL 事件。队列固定 16 项，每项最多 191 字节正文；满队列或过长输入返回 FULL/TRUNCATED，由 transport 处理交付失败，不覆盖旧消息，也不回退到接收线程同步执行。没有配置 worker 的 `post` 返回 INVALID_STATE。同步 AT 解析仍走原有内部路径；transport 负责命令响应与异步通知的分流，不应把同一行重复投递到两个路径。

worker 从 provider init 存活到 deinit。销毁前必须停止并等待外部 API/RX 调用退出；deinit 在 Modem 锁外关闭队列并 join worker，停止时尚未处理的通知可以丢弃。deinit 返回错误时必须保留整个实例并重试，不能释放仍被 worker 引用的资源。worker 回调及 AT command 不能相互等待，也不能在 worker 中调用 deinit。同步轮询 transport 可不配置 worker，但调用方仍须满足原有同步入口的串行化要求。


#### Cell Locate

Cell locate 是 QuecLocator 基站定位，与卫星定位是两条独立路径：它不依赖卫星信号，室内和冷启动也能返回粗略位置，代价是每次查询都要经 packet data 访问运营商定位服务。Provider 用 `AT+QLBSCFG="token",<token>` 配置身份、`AT+QLBS` 发起单次查询，结果通过 `h2_pal_modem_cell_locate()` 返回 `h2_pal_modem_cell_location_t`。`valid = 0` 表示服务未能定位，是正常返回而非错误。何时查询、缓存多久、如何与 GNSS fix 融合都属于产品策略，不在 PAL 或 provider 内决定。

Token 由集成方通过 `h2_quectel_modem_config_t` 的 `cell_locate_token` 注入，字符串是借用的，生命周期必须覆盖 modem instance；`cell_locate_timeout_ms` 控制单次查询超时，0 取默认 60 s。Token 为 NULL 或空串时 `cell_locate` 不装配进 vtable，`get_capabilities` 不置 `H2_PAL_MODEM_CAPABILITY_CELL_LOCATE`，调用返回 `H2_PAL_ERR_UNSUPPORTED`，模组上不会出现任何 QLBS 命令。含引号、逗号或换行、或超过 127 字节的 token 会让 `h2_quectel_modem_init` 返回 `H2_PAL_ERR_INVALID_ARG`。

前置条件是 packet data 已激活，provider 不会自行拉起 PPP；数据不可用时返回 `H2_PAL_ERR_INVALID_STATE`。Token 在首次 `cell_locate` 时惰性下发一次，`close` 后重置，因此没有用到基站定位的产品完全不会发出 token。

Token 是企业身份凭据：仓库不提供默认值，也不接受把真实 token 写进代码、测试或注释。除了必须携带它的那一条 QLBSCFG 命令外，token 不进入 modem state、response buffer、错误信息和任何日志输出；模组回显 token 时该次调用按 `H2_PAL_ERR_IO` 失败。返回的坐标同样不写入日志。

### QMI8658

`motion/qmi8658` 实现 QMI8658 IMU 初始化、打开和采样。Transport object 提供 register read/write 与 sleep callback。

### FM175xx

`nfc/fm175xx` 实现 FM175xx reader、ISO 14443 Type A card activation 和 NTAG 数据读取。Transport object 提供 register I/O 与 sleep callback。

### FM17660K

`nfc/fm17660k` 实现 reader、card emulation、FIFO 与 RF protocol state。它只依赖 exact `write_reg/write_regs/read_reg/read_regs`、reset、monotonic time 和 fallible sleep。连续读写是否固定在 FIFO data register、UART command byte/read flag/echo validation 都是芯片协议，归 portable driver；I2C/SPI/UART controller、exact-byte readiness、IRQ ring、pin 和 SDK handle 归调用方。Public transport 不暴露 `fifo_read/fifo_write`、partial offset、UART type 或 libco。

## 依赖和边界

Driver 可以知道具体 device protocol 和 register，但不能知道 board pin、I2C controller instance、UART port、interrupt wiring 或 SDK handle。这些 board 差异由 BSP 组装 transport 时提供。

## 构建与测试

每个 driver 目录都是独立 Bazel package，并通过自己的 semantic target 暴露 portable driver；不能再用一个 `//libs/drivers:drivers` 聚合生产 target 隐藏实际依赖。跨 driver public-header contract 位于 `//libs/drivers/tests`，各 driver 的协议、状态机和错误路径测试仍由对应 package 或共享 tests package 执行：

```sh
bazel test //libs/drivers/...
```

## Quectel 低功耗与 SIM 热插拔

Modem 的 ACTIVE/AUTO_SLEEP 是易失策略；实际状态独立报告。当前 Quectel provider 没有可信的休眠状态传感器，`get_power_status` 始终报告 UNKNOWN，也不发送 AT 或为查询唤醒模组。成功设置 AUTO_SLEEP 只表示允许空闲休眠。关闭后策略回到 ACTIVE；意外 RDY 后在下一次功能调用恢复配置。AT 或 sleep gate 失败会禁止自动释放唤醒，调用方可重试设置策略；尚未确认结束的通话、GNSS 和数据会话仍须成功停止。

支持范围显式限定为 EC25 UART profile，并在准备时通过 CGMM 校验型号。未声明 profile、缺少独立 command channel、sleep gate 或 PAL recursive mutex 时不发布 LOW_POWER capability，调用返回 UNSUPPORTED。其他 Quectel 系列、USB-only 接线及 SIMCom 等 provider 不因通用 capability 配置而获得低功耗支持。独立 command channel 可以是由 transport 维护的 CMUX AT DLCI；PPP 数据 DLCI 活跃期间保持唤醒，不依赖未经验证的 CMUX/PPP 休眠行为。

[EC25 Hardware Design V2.4](https://quectel.com/content/uploads/2024/02/Quectel_EC25_Series_Hardware_Design_V2.4-4.pdf) §3.4–3.5.1.1 说明普通 sleep 保留网络寻呼及语音来电，UART 场景用 QSCLK 与 DTR 配合，DTR 拉低唤醒，RI 通知主机。Provider 使用 QSCLK 0/1，不使用关闭 RF/SIM 的 CFUN 模式。Board 的 sleep gate 负责 DTR 电平、唤醒后 transport 就绪等待、所有 DLCI 排空、RI 唤醒及无损 URC 接收；存在 USB、WAKEUP_IN 或 AP_READY 时还要满足该板接线条件。官方资料未给出适用于所有固件和接线的固定唤醒延迟，因此 portable provider 不硬编码通用毫秒值。

完整 PAL 操作和 public PPP/prepare 入口共享 recursive mutex。定位的 token 配置与查询不会被关闭或策略更新穿插；GNSS 从启动到停止、通话从拨号/来电到挂断或结束 URC、PPP 从拨号到停止均保持活动。持续活动期间即使策略是 AUTO_SLEEP，也不会允许休眠。Cell locate 仍要求调用方先使 packet data 可用，provider 不建立 PPP；它依赖 QuecLocator 的结果/CME 错误判断数据不可用，不能把本地 PPP 缓存当作模组内部 PDP 激活证明。

[EC25/EC21 AT Commands Manual V1.3](https://quectel.com/content/uploads/2021/03/Quectel_EC25EC21_AT_Commands_Manual_V1.3.pdf) §5.10–5.11、§13.5 定义 QSIMDET、QSIMSTAT 和 QSCLK。启用热插拔需明确 SIM_DET 插入有效电平并提供 host data invalidation callback。准备流程读取 QSIMDET；配置不一致时写入期望值并返回 INVALID_STATE，集成方须重启模组、销毁旧 instance 并重新初始化。Provider 不自动重启或写产品偏好。已匹配时启用 QSIMSTAT 通知，命令失败则 open 失败。

QSIMSTAT 的 absent、inserted、unknown 结合 CPIN 的 READY、SIM PIN/PUK、NOT READY 复用 MODEM_SIM_CHANGED；插入通知本身不等于 READY。重复状态被合并；拔出后的 NOT READY 不覆盖已知 ABSENT。无卡、锁卡或失效状态会清除 provider 的数据/IP 状态并触发 host invalidation callback，旧 PPP link-up 回调必须被 consumer 丢弃。重新插入只通知状态，不自动拨号，也不改写用户 4G 开关。接收侧应在任务中投递完整 URC；command callback 等待期间不能同步等待另一个调用 provider 的 URC worker，否则会形成锁循环。ISR 只负责缓存/唤醒。

Consumer 集成需要同时完成：

- 转发新增 PAL power 操作和 capability，提供实际 EC25 profile、DTR/RI/transport callback；主控深睡时由私有 board 保留 Modem 电源域和唤醒线路。
- PPP adapter 在 SIM invalidation 时撤销旧 IP/DNS、取消旧 generation 回调并异步清理 netif；所有拨号和停止经过 provider 的 PPP 入口，不能绕过活动保持。
- H106 根据 Wi-Fi 优先和用户期望设置策略，拔卡不复用“用户关闭 4G”的持久化动作；重新插卡按产品期望恢复。
- 实机核验空闲电流、来电 RI/URC、主控深睡唤醒、DTR 时序、CMUX/PPP、GNSS 与反复插拔。Host mock 和并发测试不能替代这些硬件验收。
