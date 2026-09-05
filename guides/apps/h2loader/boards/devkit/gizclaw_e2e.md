# DevKit GizClaw E2E

`projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit` 把 `projects/e2e/apps/gizclaw/app` 持有的 portable registry 编译成 board `devkit`、target `esp32s3`、role `app`、image `gizclaw-e2e` 的 H2Loader package。Launcher 使用 DevKit Runtime、H2Peer-backed WebRTC PAL、北京 E2E 入口和 RuntimeProfile `default` 自己的固定 RegistrationToken；不运行 H106 产品逻辑，也不访问显示、按键、麦克风或扬声器。

## Runtime Lifecycle

Launcher 先初始化 Runtime 和 H2Loader App command service，再启动独立 Wi-Fi supervisor。Supervisor 从 Runtime `wifi_settings` 读取 Loader 已确认并保存的 STA 配置；没有 saved config 时每 10 秒报告一次 `NO_SAVED_WIFI`，连接失败或断开后同样等待 10 秒重试。日志不输出 SSID、Wi-Fi password、RegistrationToken、Firmware URL、原始音频或 unrestricted response body。

`deploy-default` 是只绑定 RuntimeProfile `default` 的公开测试 identity，仍由 launcher-private config 固定进 image。Wi-Fi SSID 和 password 不编进 package；安装或重启后使用 ESP Wi-Fi settings PAL 持久化的配置。修改 endpoint 或 token 必须重新构建并通过 H2Loader 安装新 package；修改 Wi-Fi 只需执行 `reboot loader` 返回 Loader，成功执行 `wifi connect`，再执行 `reboot app` 启动 App。

主循环消费 Runtime system event。第一次收到 Wi-Fi `GOT_IP` 后创建唯一 runner，调用一次 `h2_gizclaw_e2e_run()` 的 `all` suite；后续断线和重连只更新连接状态，不重新运行。portable App 继续执行所有独立 case、反向 cleanup 和 terminal aggregation，不因单个错误提前退出。运行期间至少每 10 秒输出进度，完成后立即输出一次 summary，并每 10 秒重放同一 bounded final summary，便于晚接入 UART 的操作者取得结论。

APP 在 Runtime、command service、Wi-Fi supervisor 和报告循环成功启动后完成 Stage 收尾。Wi-Fi、C SDK、server 或 E2E case 失败不会自动切换分区。要重新执行完整 suite，使用 `reboot app --monitor` 建立新的 boot boundary；只查看剔除 iKCP frame 后的日志可使用独立 `monitor`。

## Build

```sh
bazel test --config=macos_arm64 \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit:gizclaw_e2e_devkit_state_test \
  //libs/pal/providers/h2peer:all \
  //native_component_src/esp-idf6.x/h2_pal_core:dtls_state_test \
  //native_component_src/esp-idf6.x/h2_pal_core:net_socket_test
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit:package
```

Package 输出为：

```text
bazel-bin/projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit/package/
└── devkit-gizclaw-e2e-esp32s3.update.tar.zlib
```

## Managed Device Acceptance

先按 [CLI 使用说明](/zh/using/h2loader/cli) scan 并验证实时 identity 是 `board=devkit`、`target=esp32s3`。设备仍可通信时只使用 H2Loader managed flow：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> wifi connect <ssid> <password>
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> send \
  --file bazel-bin/projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit/package/devkit-gizclaw-e2e-esp32s3.update.tar.zlib
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> reboot upgrade
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> reboot app --monitor
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> coredump status
```

`wifi connect` 必须先返回 `H2_LOADER_WIFI result=connected`；缺少该步骤时 App 只能报告 `NO_SAVED_WIFI`，不能记为网络可用。最终 status 必须包含 `active_role=app`、匹配的 active/Partition 2 identity、`boot_intent=auto` 和空 Stage。UART evidence 必须包含 launcher `READY`、首次 `GOT_IP` 后唯一 `STARTED`、全部选中 case 的 terminal record、cleanup 和持续 summary replay；不得出现第二次 runner、watchdog、reset loop、新 coredump 或 credential。业务 case 可以报告 FAIL、ERROR 或 BLOCKED，但 report 不完整或 command transport 失联会阻塞验收。
