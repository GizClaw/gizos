# AMOLED Crash Before Confirm

`crash-before-confirm` 在 H2Loader command service ready 后、ESP OTA image confirm
前主动触发 panic。它用于验证未确认 APP 的 Bootloader rollback、Loader 对失败 Stage
的收尾，以及 UART/BLE coredump 流式读取；一次 panic 或重启本身不算通过。

## 构建

```sh
bazel build --config=esp32s3 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/amoled:package \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/amoled:package
```

runner 通过运行时 `--crash-firmware` 接收 APP 包，不把设备路径或 BLE ID 编译进
binary：

```sh
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <port> \
  --ble-id <id> \
  --expected-board amoled \
  --expected-target esp32s3 \
  --crash-firmware <amoled-crash-before-confirm-esp32s3.update.tar.zlib> \
  --timeout-ms 120000 \
  --report <report.json>
```

## Partition layout

该 target 使用 `boards/amoled/esp32s3/layouts/h2loader/partition.csv`：OTA_0
`h2loader` 为 2 MiB Partition 1，OTA_1 `app` 为 8 MiB Partition 2，另有 2 MiB
`dl`、2 MiB `data`、64 KiB `coredump` 与 256 KiB `pref`。APP Stage 包位于
`dl`，panic 数据写入独立 coredump partition。

## sdkconfig

`boards/amoled/esp32s3/sdkconfig.defaults` 启用 16 MiB Flash、USB Serial/JTAG
console、panic print/reboot 与 flash coredump；最多记录 16 个 task，coredump stack
为 2048 bytes。H2Loader layout 另启用自定义 partition table 和 ESP APP rollback。
managed UART 启动默认值为 230400。

## 验收

runner 只发送一次 crash APP Stage 并执行 `reboot upgrade`。ESP Bootloader 必须把
未确认的 Partition 2 标记为 `INVALID` 或 `ABORTED` 并回退 Partition 1；OTA 状态
查询失败必须作为错误向上传递，不能冒充 rollback。Loader 只在明确看到失败的
Partition 2 时清理匹配 Stage、记录 `last_result=INVALID_STATE`，且不得再次写入同一
APP 形成 crash loop。

随后 UART 和 BLE 都必须通过 `coredump status`、完整 `dump`、`erase` 与清空后的
status。dump 按流式 chunk 写入 Host，不受单个响应缓冲区大小限制；报告中的实际
字节数、终态、重连次数和最终 P1/P2/Stage metadata 都必须来自设备响应。

2026-08-31 的 AMOLED 实板运行在完整 erase 和重新刷入当前 Loader 后通过 19/19：
自动 rollback 与 Stage 收尾通过，UART 和 BLE 均完整读取 23,264 bytes coredump，
随后 erase 和空状态复查通过。
