# Drivers

`libs/drivers` 保存与具体 board 和 SDK 无关的芯片、传感器及外设 driver。Driver 负责设备协议和状态机，实际总线、GPIO、UART、sleep 和同步能力由调用方注入。

## API Reference

[API Reference](/references/drivers)

`libs/drivers` 各 driver 的 `include/` 目录中实际参与项目构建的头文件是 Drivers 的生产 Public API contract。

## Driver Families

### Quectel Modem

`modem/quectel` 实现 Quectel modem 的 AT command、URC、call、GNSS、PPP 和状态处理，并输出 `h2_pal_modem_api_t`。Config 注入 transport callback、PAL sync、mem 和 system event API。

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
