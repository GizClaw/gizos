# DevKit H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_zlib_tar/loader/devkit:package \
  //projects/h2loader/targets/h2loader_zlib_tar/loader/devkit:package_uart_460800 \
  //projects/h2loader/targets/h2loader_zlib_tar/e2e-app/devkit:package
```

默认 `package` 使用 USB Serial/JTAG console；`package_uart_460800` 使用 UART0 460800 console。每个 DevKit firmware Bazel target 必须通过 `console = "usb"` 或 `console = "uart"` 显式选择 profile，CMake 只消费 Bazel 传入的 defaults 文件，不自行猜测 console。内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 managed package 和 release metadata；ESP-IDF app descriptor 和 package manifest 使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

USB Serial/JTAG 不使用 UART baud；CLI 的 `--baud` 参数不会改变它的 USB 链路速率。UART profile 固定为 460800，调用 CLI 和 E2E runner 时必须显式传入 `--baud 460800`。Host open 后先 deassert DTR/RTS；native USB Serial/JTAG provider 返回 canonical `UNSUPPORTED` 时继续，任何其它控制线错误终止连接。APP 与 Loader status 都从设备端 BLE public/identity MAC 返回 12 位小写十六进制 `device_uid`；BLE endpoint 只负责发现，任何重启后的连接都必须先匹配 UID。

## sdkconfig

`boards/devkit/esp32s3/layouts/h2loader/loader_usb.defaults` 启用 native USB Serial/JTAG console，不设置 monitor baud。`loader_uart.defaults` 启用 UART0 custom console，并把 console 与 ESPTool monitor baud 都设为 460800。Console profile 不放在 board-wide `sdkconfig.defaults`，避免 board defaults 隐式覆盖 Bazel target 的选择。

## 预期表现

先运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan`，只选择结构化 identity 为 `board=devkit`、`target=esp32s3`、`active_role=loader`、`transport=iostreamikcp` 的设备。随后用 scan 返回的 port 执行 `status`，确认没有有效 Stage，再使用 `send --file <build-dir>/update.tar.zlib` 和 `reboot upgrade` 完成 Loader self-update。

更新流程、完成条件和断电复查见 [Loader 更新](../../update/loader)。已经安装 H2Loader 的正常路径不直接烧录；只有 self-update 无法执行、重新确认 board/target identity 且已获得 destructive recovery 授权时，才按恢复流程使用本 target 的 `.recovery.h2fb`。

## E2E runner

设备参数只通过 `bazel run` 后的 runtime flags 传入，不编译进 runner：

```sh
H2LOADER_E2E_WIFI_PASSWORD='<password>' \
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <serial-endpoint> \
  --ble-id <ble-endpoint> \
  --expected-board devkit \
  --expected-target esp32s3 \
  --app-firmware <devkit-e2e-app-esp32s3.update.tar> \
  --loader-firmware <devkit-loader-esp32s3.update.tar> \
  --firmware-url <device-reachable-url> \
  --url-bytes <bytes> \
  --url-sha256 <sha256> \
  --wifi-ssid <ssid> \
  --wifi-password-env H2LOADER_E2E_WIFI_PASSWORD \
  --monitor-ms 5000 \
  --report <report.json>
```

四种 checksum 组合的双格式 fixture 由旧 E2E App entry 的 `:checksum-matrix` 生成；runner 的 `--checksum-tar-zlib DIR` 与 `--checksum-zlib-tar DIR` 可同时或分别选择两条轨道。它们复用真实 Stage/upgrade/reconnect 流程，并用设备 `stats` 的 installed data checksum 校验 data-only 与 both-changed 的实际结果。具体输入、guarded skipped streams 和报告合同见 [E2E runner README](https://github.com/GizClaw/gizos/blob/main/projects/h2loader/apps/e2e-runner/README.md)。
