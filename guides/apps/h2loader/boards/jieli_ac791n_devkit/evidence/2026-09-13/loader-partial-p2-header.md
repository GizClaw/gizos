# P2 启动头部分写入后的断电恢复

## 范围

设备 `3ce9e275d7aa`，UART `/dev/cu.usbserial-20131240`，460800。
本次只覆盖原生 P2 启动头的 16 字节前缀写入；不代表所有损坏模式、P1
启动头写入中断或 Preference 写入中断已经通过。

诊断组件 `loader_partial_header` 仅链接到独立、禁止发布的测试包。
它只在 P2 运行且 P1 启动头 CRC 有效时操作 P2：写入 32 字节启动头的前
16 字节，读回确认后 16 字节仍为擦除值，并确认部分头的 CRC 无效；关闭
看门狗并持续输出 `H2_JIELI_PARTIAL_HEADER_READY`，等待真实断电。
不会擦除 P1。主机单测覆盖运行分区、读取失败、P1 CRC 和写入范围守卫；
单测中的解码是 mock，不代替下述硬件结果。

## 镜像身份

| 镜像 | SHA-256 |
| --- | --- |
| 正式 v5 包 | `cada36c7785de5983a11277d60152799f26040458e072146bc39dfd0de6794e8` |
| 正式 v5 镜像 | `fea3044e4e19b829f2ecc4504b0da7d691b3dc3397a1258c0faf7f2ff82d068d` |
| 部分头诊断包 | `b509820e83f3e552a24344674172fee39a1f63d5fd336024aa5869f3329830be` |
| 部分头诊断镜像 | `206b99b81eb2e99797bab33004669ef3fa0fca7d45540c1945845fb8c0965000` |

## 真实断电及恢复结果

1. 诊断候选输出 `H2_JIELI_PARTIAL_HEADER_READY bank=2 prefix=16 p1_crc=valid`。
2. 用户按提示断电、重新上电后，原始 `partial-header-v1-monitor.log`
   第 2577 行记录 `reset reason: POWER ON`，随后捕获到 Loader 启动及 UART 心跳。监控最终
   返回 `code=-7`，因此没有把监控退出当成设备状态证据。
3. 独立 `status` 返回成功：`running_partition=1`、正式 v5 镜像身份、
   `stage_valid=1`；没有启动部分头的 P2 候选。
4. 不进入 USB DL，通过 UART 发送正式 v5 包，`send` 返回 OK，916907 字节
   与包 SHA 匹配；执行 `reboot upgrade --monitor`。
5. 写入日志：`H2_JIELI_UPDATE_WRITE_DONE expected=928573 native=928573 result=0`；
   完整镜像摘要匹配正式 v5，随后 `H2_JIELI_STARTUP_EVENT event=4 code=0`。
6. 停止监控后独立查询：`running_partition=1`，P1/P2 元数据均为正式 v5，
   `stage_valid=0`、`last_result=0`，UART 命令可用。

本次重装镜像与原 P1 相同，因此证明的是损坏 P2 可重写、校验及清理 Stage，
不是一次不同身份候选的试运行或 P2→P1 搬运。原始本地采集在
`tmp/jieli/partial-header-v1-monitor.log` 和
`tmp/jieli/partial-header-recovery-v5-monitor.log`；这些临时日志不属于发布包。
