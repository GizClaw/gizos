# NTP

`libs/ntp` 提供跨平台 NTP packet codec 和时间同步 client。

## API Reference

[API Reference](/references/ntp)

`libs/ntp/include` 中实际参与项目构建的头文件是 NTP 的生产 Public API contract。同步结果包含 server time、local monotonic receive time、round trip、offset，以及是否成功写入 wall clock。

## 依赖和边界

NTP 不直接访问 socket 或系统时钟。Server、bind、timeout、retry 和是否设置 wall clock 由调用方通过 config 决定。

冷启动时 PAL wall clock 可以报告 `H2_PAL_TIME_ERR_UNCALIBRATED`。Client 用 monotonic milliseconds 作为合成的 Unix-valued 请求／接收 reference，参与 transaction 校验、NTP era 选择与 offset 计算；不把该 reference 写为 UTC。Packet decoder 在相邻 era 中选择离 reference 最近的非负 Unix 时间，距离相同时选较早者。正常开机 uptime 对应首个 1970 年之后的 timestamp，包含 2036 年 rollover；完全相同的后续 era timestamp 按此政策折回，已有有效 wall clock 时则用该日期解析后续 era。没有日历锚点时不能从 32 位秒字段识别任意未来 era。

`server_unix_ms` 是上述政策解析的服务器发送时间；`offset_ms` 是相对于接收 reference 的校正量，冷启动时不表示已有 UTC 时钟的误差。`round_trip_ms` 使用 monotonic 往返耗时减去服务器处理时间，负值截为零；设置成功时 `applied_wall_ms` 是接收 reference 加 offset，未设置时为零。来源、originate、服务器状态和非零 server timestamp 仍须通过验证，非法响应不设置时钟。其它 clock provider 错误仍直接失败。依赖证书时间的 HTTPS consumer 必须等校时成功且 wall clock 可读后再连接。

## 构建与测试

```sh
bazel test //libs/ntp:all
```
