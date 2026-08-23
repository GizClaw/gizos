# NTP

`libs/ntp` 提供跨平台 NTP packet codec 和时间同步 client。

## API Reference

[API Reference](/references/ntp)

`libs/ntp/include` 中实际参与项目构建的头文件是 NTP 的生产 Public API contract。同步结果包含 server time、local monotonic receive time、round trip、offset，以及是否成功写入 wall clock。

## 依赖和边界

NTP 不直接访问 socket 或系统时钟。Server、bind、timeout、retry 和是否设置 wall clock 由调用方通过 config 决定。

## 构建与测试

```sh
bazel test //libs/ntp:all
```
