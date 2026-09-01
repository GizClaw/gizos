# DevKit H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/devkit:package \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/devkit:package_uart_460800 \
  //projects/h2loader/targets/h2loader_tar_zlib/e2e-app/devkit:package
```

默认 `package` 使用 USB Serial/JTAG console；`package_uart_460800` 使用 UART0 460800 console。每个 DevKit firmware Bazel target 必须通过 `console = "usb"` 或 `console = "uart"` 显式选择 profile，CMake 只消费 Bazel 传入的 defaults 文件，不自行猜测 console。内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 managed package 和 release metadata；ESP-IDF app descriptor 和 package manifest 使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

USB Serial/JTAG 不使用 UART baud；CLI 的 `--baud` 参数不会改变它的 USB 链路速率。UART profile 固定为 460800，调用 CLI 和 E2E runner 时必须显式传入 `--baud 460800`。Host open 后先 deassert DTR/RTS；native USB Serial/JTAG provider 返回 canonical `UNSUPPORTED` 时继续，任何其它控制线错误终止连接。APP 与 Loader status 都从设备端 BLE public/identity MAC 返回 12 位小写十六进制 `device_uid`；BLE endpoint 只负责发现，任何重启后的连接都必须先匹配 UID。

## Partition layout

待补充 H2Loader image 使用的 partition table 和各分区用途。

## sdkconfig

`boards/devkit/esp32s3/layouts/h2loader/loader_usb.defaults` 启用 native USB Serial/JTAG console，不设置 monitor baud。`loader_uart.defaults` 启用 UART0 custom console，并把 console 与 ESPTool monitor baud 都设为 460800。Console profile 不放在 board-wide `sdkconfig.defaults`，避免 board defaults 隐式覆盖 Bazel target 的选择。

## 预期表现

先运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan`，只选择结构化 identity 为 `board=devkit`、`target=esp32s3`、`active_role=loader`、`transport=iostreamikcp` 的设备。随后用 scan 返回的 port 执行 `status`，确认没有有效 Stage，再使用 `send --file <build-dir>/update.tar.zlib` 和 `reboot upgrade` 完成 Loader self-update。

命令成功只表示重启请求已被接受。验收还必须等待 Partition 2 候选 Loader 启动并回写，重新连接后执行 `status`，确认 active version 等于构建版本、运行 Partition 1、`boot_intent=AUTO`、Partition 1/2 valid 且 image checksum 相同、Stage invalid，并在 power-cycle 后再次确认。已经安装 H2Loader 的正常路径不直接烧录；只有 self-update 无法执行、重新确认 board/target identity 且已获得 destructive recovery 授权时，才按恢复流程使用本 target 的 `.recovery.h2fb`。

## E2E runner

设备参数只通过 `bazel run` 后的 runtime flags 传入，不编译进 runner：

```sh
H2LOADER_E2E_WIFI_PASSWORD='<password>' \
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <serial-endpoint> \
  --ble-id <ble-endpoint> \
  --baud 115200 \
  --expected-board devkit \
  --expected-target esp32s3 \
  --app-firmware <devkit-e2e-app-esp32s3.update.tar.zlib> \
  --loader-firmware <devkit-loader-esp32s3.update.tar.zlib> \
  --firmware-url <device-reachable-url> \
  --url-bytes <bytes> \
  --url-sha256 <sha256> \
  --wifi-ssid <ssid> \
  --wifi-password-env H2LOADER_E2E_WIFI_PASSWORD \
  --monitor-ms 5000 \
  --report <report.json>
```

2026-08-31 的 PR #64 head `b6370c4` 在当时的 230400 contract 下完成实板验收；当前默认 contract 已改为 115200：UART
31/31 PASS、BLE 28/28 PASS，合计 59/59 PASS。Loader 与 APP 环境都分别覆盖
`help/status/stats/memory`、旧命令从 help 与 availability 消失、Wi-Fi
scan/connect/disconnect、payload/URL Stage 和两种 abort；每个 APP case 的 authoritative
status 都确认 `active_role=app`。生命周期还覆盖 APP 安装、普通 reboot 不消费 Stage、Loader
P2-to-P1 copy-back；UART 另覆盖 standalone monitor 及三种 `reboot ... --monitor`。最终 UART
与 BLE status 均为 Loader `pr64-b6370c4` / Partition 1、`boot_intent=AUTO`、Stage
invalid，Partition 1/2 都持有同一个 935403-byte Loader package checksum
`c339a76f587c12900f258981fa3e15e01aac536c4d64a5c8d21b1e334f9ca8ec` 和 image
checksum `8d2d88b5b52c4f1ea17315a4f4fb409880a323f5270cd2b9af8ea405c29193c6`。

独立的预置 coredump fixture 报告为 UART/BLE 10/10 PASS，每种传输都流式读取 23200
bytes 后 erase；它证明大于 8 KiB 的输出不会再聚合到固定响应缓冲区。
