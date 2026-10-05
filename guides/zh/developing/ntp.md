# NTP

`libs/ntp` 提供跨平台 NTP packet codec 和时间同步 client。

## API Reference

[API Reference](/references/ntp)

`libs/ntp/include` 中实际参与项目构建的头文件是 NTP 的生产 Public API contract。同步结果包含 server time、local monotonic receive time、round trip、offset，以及是否成功写入 wall clock。

## 依赖和边界

NTP 不直接访问 socket 或系统时钟。Server、bind、timeout、retry 和是否设置 wall clock 由调用方通过 config 决定。

冷启动时 PAL wall clock 可以报告 `H2_PAL_TIME_ERR_UNCALIBRATED`。Client 此时只用 monotonic time 生成请求 transaction timestamp 和计算往返耗时，不把启动计时器写为 UTC；收到来源、originate timestamp 和服务器状态都有效的响应后，才按调用方的 `set_wall_clock` 配置写入真实 UTC。其它 clock provider 错误仍直接失败。依赖证书时间的 HTTPS consumer 必须等校时成功且 wall clock 可读后再连接。

## 构建与测试

```sh
bazel test //libs/ntp:all
```
