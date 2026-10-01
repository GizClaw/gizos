# DevKit GizClaw E2E

`projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit` 把 `projects/e2e/apps/gizclaw/app` 持有的 portable registry 编译成 board `devkit`、target `esp32s3`、role `app`、image `gizclaw-e2e` 的 H2Loader package。Launcher 使用 DevKit Runtime、H2Peer-backed WebRTC PAL 和显式选择的受控 E2E endpoint、RuntimeProfile、RegistrationToken、AppConfig key/value 以及非敏感 HTTPS 输入；不运行 H106 产品逻辑，也不访问显示、按键、麦克风或扬声器。

## Runtime Lifecycle

Portable E2E 的并发 run guard 由每对象普通 static backing 提供，launcher 无需为它增加启动和退出调用；仍在资源全部清理后解除 guard，retained session 保持占用直到 image 退出。

Launcher 先初始化 Runtime 和 H2Loader App command service，再启动独立 Wi-Fi supervisor。Supervisor 从 Runtime `wifi_settings` 读取 Loader 已确认并保存的 STA 配置；没有 saved config 时每 10 秒报告一次 `NO_SAVED_WIFI`，连接失败或断开后同样等待 10 秒重试。日志不输出 SSID、Wi-Fi password、RegistrationToken、Firmware URL、原始音频或 unrestricted response body。

`gizclaw_e2e_fixture` macro 或对应 Bazel build settings 提供私有 token、公开 profile/key/已知 value、HTTPS Device API/audio URL 和 Time server。缺少输入在网络注册和远端资源变更前失败。token 不进入提交的默认值、结构化 receipt 或日志；Wi-Fi 凭据继续从已有 Settings 借用。构建结果只保留在私有本地环境。

主循环消费 Runtime system event。第一次收到 Wi-Fi `GOT_IP` 后创建唯一 runner，调用一次 `h2_gizclaw_e2e_run()` 的 `all` suite；后续断线和重连只更新连接状态，不重新运行。portable App 继续执行所有独立 case、反向 cleanup 和 terminal aggregation，不因单个错误提前退出。运行期间至少每 10 秒输出进度，完成后立即输出一次 summary，并每 10 秒重放同一 bounded final summary，便于晚接入 UART 的操作者取得结论。

APP 仅在业务用例、反向清理和 runner join 全部成功、无 retained owner、完整有界账本冻结后完成确认与 Stage 收尾。384 KiB 账本记录当前 image version、随机 execution nonce、逐条 API evidence、完整 summary、CRC32 和实际确认结果；迟接入 UART 可以读取同一个 boot 的完整重播。失败保持未确认，不能把空 coredump 当成没有 reset 的证明。要重新执行完整 suite，使用 `reboot app --monitor` 建立新的 boot boundary；只查看剔除 iKCP frame 后的日志可使用独立 `monitor`。

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

定位长寿命 Peer 的 channel 生命周期时，可给同一 package build 添加 `--define=H2_GIZCLAW_E2E_CONCURRENCY_ONLY=1`，只运行 concurrency suite。它保持单个 client/Peer，执行 32 批六个并发 Ping 和每批一次恢复 Ping；每批必须回收全部 RPC channel，最终输出 `stage=channel-soak batches=32/32 requests=192 result=PASS rc=0`，随后仍执行资源 cleanup。该选项不能与 `H2_GIZCLAW_E2E_VOICE_ONLY` 同时启用。

Package 输出为：

```text
bazel-bin/projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit/package/
└── devkit-gizclaw-e2e-esp32s3.update.tar.zlib
```

## Managed Device Acceptance

在已明确移交的独占端口定向读取 H2Loader status，验证 `board=devkit`、`target=esp32s3`、device UID、P1、Stage 和真实 coredump 基线；并行 agent 工作期间不使用 scan。设备仍可通信时只使用 H2Loader managed flow：

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

全量资格要求 managed 启动与独立 normal App boot 各自8/8、227项审计、cleanup/retained=0。第二次执行必须生成新的 nonce；相同 boot 的重播不能替代它。最终 status 必须匹配 package/image，Stage 空、running/next=App、原 P1 不变且实际 crash 基线不变。软件 PCM/speaker delegate 只验收业务路径，不构成麦克风或声学资格。
