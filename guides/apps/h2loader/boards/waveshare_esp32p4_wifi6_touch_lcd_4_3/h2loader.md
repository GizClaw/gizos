# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 H2Loader

## 构建

```sh
bazel build --config=esp32p4 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

首次恢复或已经确认 H2Loader 无法通信时，只能选择这个 target 生成的 `waveshare_esp32p4_wifi6_touch_lcd_4_3-h2loader-esp32p4.recovery.h2fb`，重新匹配实际 P4 port、确认 board/target identity、取得 destructive recovery 授权并执行恢复。

正常更新路径使用同 target 生成的 `update.tar.zlib`，不直接烧录。

## Partition layout

P4 的 32 MB flash 中，当前布局使用前 14.2 MB：包含 2 MB `h2loader`、8 MB `app`、各 2 MB 的 `/dl` 与 `/data`，以及 64 KB `coredump`。`nvs`、`otadata` 和 `phy_init` 位于 app partition 之前，其余空间保留。

## sdkconfig

P4 启用 octal PSRAM、ESP-Hosted SDIO、remote Wi-Fi、NimBLE host、extended advertising 与 Hosted VHCI；Hosted transport mempool 和默认 worker task 使用 PSRAM，避免完整 Loader 启动时耗尽内部 RAM。本地 Bluetooth controller 禁用；PAL 在 NimBLE 启动和停止时通过 Hosted RPC 对称管理 C6 Bluetooth controller。`app_main()` 只创建独立的 Board entry task，因此 ESP-IDF main task stack 固定为 8 KiB，避免在 PSRAM/XIP mapping 完成后为 64 KiB startup stack 申请 internal RAM 失败。UART0 console 和 UART iKCP 使用固定 `115200` baud。panic coredump 写入 flash，最多记录 16 个 task。

## 预期表现

C6 ready 后，Loader 通过 UART 与 BLE iKCP 接受 command session。`bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 的结构化 identity 必须为 `board=waveshare_esp32p4_wifi6_touch_lcd_4_3`、`target=esp32p4`、`active_role=loader`，服务 ready 时输出 `H2_LOADER_READY target=esp status=ready`。

Hosted 初始化失败不得伪装成 Wi-Fi/BLE ready；保留 UART session 用于查询状态和恢复。正常 Loader 更新使用 `send` 和 `reboot upgrade`，验收覆盖 Partition 2 候选 Loader 启动和 Partition 1 回写；最终 `status` 必须显示目标 version、运行 Partition 1、`boot_intent=AUTO`、Partition 1/2 valid 且 image checksum 相同、Stage invalid，并在 power-cycle 后复查。
