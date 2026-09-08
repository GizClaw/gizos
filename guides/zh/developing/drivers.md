# Drivers

`libs/drivers` 保存与具体 board 和 SDK 无关的芯片、传感器及外设 driver。Driver 负责设备协议和状态机，实际总线、GPIO、UART、sleep 和同步能力由调用方注入。

## API Reference

[API Reference](/references/drivers)

`libs/drivers` 各 driver 的 `include/` 目录中实际参与项目构建的头文件是 Drivers 的生产 Public API contract。

## Driver Families

### Quectel Modem

`modem/quectel` 实现 Quectel modem 的 AT command、URC、call、GNSS、cell locate、PPP 和状态处理，并输出 `h2_pal_modem_api_t`。Config 注入 transport callback、PAL sync、mem 和 system event API。

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
