# DevKit H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/devkit:package \
  //projects/h2loader/targets/h2loader_tar_zlib/e2e-app/devkit:package
```

USB Serial/JTAG 是 canonical console；provider 只接受 launcher 声明的 allowlisted build variables，不能用文档暴露第二套 native build tree。内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 managed package 和 release metadata；ESP-IDF app descriptor 和 package manifest 使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

## Partition layout

待补充 H2Loader image 使用的 partition table 和各分区用途。

## sdkconfig

待补充 USB Serial/JTAG 与 UART console 对应的 `sdkconfig` defaults 和关键配置。

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

2026-08-31 的 PR #64 当前源码实板验收使用 115200：UART 19/19 PASS，BLE
16/16 PASS。两者都覆盖 `help/status/stats/memory`、旧命令从 help 与 availability
消失、Wi-Fi scan/connect/disconnect、payload/URL Stage、两种 abort、APP 安装、普通
reboot 不消费 Stage 和 Loader P2-to-P1 copy-back；UART 另覆盖 standalone monitor 及三种
`reboot ... --monitor`。最终状态均为 Loader / Partition 1、Stage invalid、Partition 1/2
valid 且 Loader package/image checksum 相同。独立的预置 coredump fixture 报告为 UART/BLE
10/10 PASS，每种传输都流式读取 23200 bytes 后 erase；它证明大于 8 KiB 的输出不会再聚合到
固定响应缓冲区。
