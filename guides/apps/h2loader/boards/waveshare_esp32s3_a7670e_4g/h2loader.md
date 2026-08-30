# Waveshare ESP32-S3-A7670E-4G H2Loader

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/waveshare_esp32s3_a7670e_4g:package
```

构建生成 `h2loader.bin` 和 `update.tar.zlib`。UART0 console 固定为启动默认值 `115200` baud，BLE iKCP 同时可用于 H2Loader command transport。

首次恢复或已经确认 H2Loader 无法通信时，只使用 USB identity 确认属于当前 Board 的 WCH port，重新确认 board/target identity，并以这个 target 生成的 `.recovery.h2fb` 取得 destructive recovery 授权后恢复。

正常安装、更新、恢复和回滚使用 H2Loader command transport，不直接烧录。

## Partition layout

16 MiB flash 包含 2 MiB `h2loader`、8 MiB `app`、2 MiB `/dl`、2 MiB `/data` 和 64 KiB `coredump`。`nvs`、`otadata` 和 `phy_init` 位于 app partition 之前；`app` 容量大于 `h2loader`，可以承载 Loader self-upgrade 的 trial image。

## sdkconfig

Board defaults 启用 16 MiB/80 MHz flash、8 MiB octal PSRAM、PSRAM XIP、Wi-Fi、NimBLE central/peripheral/broadcaster/observer、extended advertising、PPP PAP/CHAP 和 Modem URC handler。Loader defaults 选择 H2Loader partition table、App rollback、flash coredump 和 `115200` baud UART0 console。

## 预期表现

用 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 只选择结构化 identity 为 `board=waveshare_esp32s3_a7670e_4g`、`target=esp32s3`、`active_role=h2loader` 的设备。完成证据必须来自设备重启后重新连接，而不是 package 传输成功：

- App 安装：`active_role=app`、`active_name` 符合预期、`state=confirmed`，并且 installed checksum 与 package 一致。
- Loader self-update：`active_role=h2loader`、`active_name=h2loader`、`active_version` 符合预期，`upgrade_phase=idle`、`upgrade_last=0`，并且 staged package 已清除。Loader status 当前不提供 installed checksum，不能把 App 的 checksum 条件套用于 Loader self-update。
