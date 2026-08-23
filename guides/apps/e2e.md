# E2E 测试 App

`projects/e2e` 持有没有其他产品 owner、可以由 Desktop 与真实设备 entry 复用的 headless 测试 App。这里的 App 以机器可验证的 case、结果和清理合同验证 Runtime/PAL 与目标 library 的集成；启动完整 production Main App、依赖产品页面与 policy 的 E2E 属于对应产品的 `projects/<product>/apps/e2e/app`。用于向人展示能力组合的 runnable 场景仍属于 [Examples](/apps/example)，library-local unit、fake、parser 和 protocol test 仍属于对应 `libs/<library>/tests`。

## Ownership

```text
projects/e2e/
├── apps/<test-app>/
│   ├── app/
│   │   ├── include/                       # stable blocking App contract
│   │   ├── src/                           # target-independent case registry
│   │   └── tests/                         # deterministic App-local tests
│   ├── data/                              # deterministic non-user fixture, optional
│   └── README.md                          # observable test contract
└── <artifact-rule>/<image>/<board>/       # standalone E2E artifact without another product owner
```

Portable E2E App 可以依赖 Runtime、PAL 与被测 target-independent library；不能依赖 Desktop、OS、Board、SDK、H2Loader、process environment、host filesystem path 或具体 backend。App 持有 case ID、执行顺序、assertion、deadline、progress、non-fail-fast aggregation、result schema 与 App-owned cleanup。

Platform artifact entry 持有 Runtime assembly、具体 provider、endpoint 与 fixture 注入、platform lifecycle 和结果输出。H2Loader-managed E2E image 位于 `projects/e2e/targets/h2loader_tar_zlib/<image>/<board>`；`h2loader_tar_zlib` 表示安装产物类型，不改变 E2E ownership。没有 H2Loader 的独立诊断 image继续位于 `projects/e2e/targets/<firmware-rule>/<image>/<board>`。

可在 host 上确定性运行、没有外部依赖的 App-local、parser、fake 和 loopback 测试属于默认自动测试，不声明 tag。连接外部服务、真实设备或要求人工准备环境的 E2E test 只声明 Bazel 特殊 tag `manual`，因此不会被通配 target pattern 自动选中。每个 `manual` test 必须有且只有一个同名 `make bazel-test-<test_name>` 入口；该入口禁用 live result cache，并直接运行对应 exact Bazel label，不再维护额外 tag filter。测试不得声明 `manual` 以外的 tag，也不创建 `test_suite` 聚合入口。

## Apps

| App | Portable target | Current launcher matrix |
| --- | --- | --- |
| GizClaw | `//projects/e2e/apps/gizclaw/app:gizclaw_e2e` | Desktop H2Peer；Desktop Pion 只比较 Firmware 与 Voice；DevKit ESP32-S3 H2Peer |
| H106 | `//projects/e2e/apps/h106/app:h106_e2e` | Desktop Tiga/Zero、Tiga V4.2 与 Zero BK 1.0；完整 production Main App、Runtime Test Control 与公开 observation |
| Libco | `//projects/e2e/apps/libco/app:libco_smoke` | Desktop、Browser、DevKit ESP32-S3、BK7258、TapDoki BK3633 |
| Lua Runtime | `//projects/e2e/apps/lua-runtime/app:lua_runtime_e2e` | Desktop、Browser、AMOLED；九个固定 VM/coroutine/component/event/worker/shutdown case |
| PAL | `//projects/e2e/apps/pal/app:pal_e2e` | Linux/macOS/Windows 共同 host OS/Filesystem/Net/TLS/CoreHTTP/CoreMQTT；Desktop core/MQTT/SQLite Preference；Browser core；DevKit 与 Tiga V4.2 H2Loader `pal-pref` |
| H2Loader Serial | `//projects/e2e/apps/h2loader-serial/app:h2loader_serial_e2e` | macOS Desktop；desktop Chrome Browser |
| WebRTC Performance | `//projects/e2e/apps/webrtc-performance/app:webrtc_performance` | Desktop H2Peer + local Pion；DevKit ESP32-S3 H2Peer + operator LAN Pion |

未来独立的 `corehttp` 与 `coremqtt` 只有在 case 和 platform matrix 已经定义时才创建。PAL App 只验证 PAL API 的跨目标公共行为，不吸收 backend-local unit、fake 或 protocol tests。Provider 名属于 launcher target；不能为了 H2Peer、Pion 或另一 backend 复制 portable case registry。H106 production App、adapter、UI 与业务 policy 继续属于 `projects/h106`；H106 E2E 的 evidence boundary 和运行合同见 产品 E2E。

## H106

`projects/e2e/apps/h106/app` 持有跨目标 case registry、bounded terminal ledger、non-fail-fast aggregation、Audio decorator、Main App supervisor 和报告合同。H106 产品组继续拥有 production Main App、产品 policy 与 Desktop/Tiga/Zero artifact entry；各 launcher 只提供 production Runtime/provider assembly、各产品自己的 checked-in RegistrationToken、固定 AP E2E endpoint、目标 memory reader 和 H2Loader lifecycle。

每个目标都启动完整阻塞式 `h2_h106_run()`；测试只通过 Runtime Test Control 注入 public component event，并且只读取公开 bounded observation。H106 E2E 不调用 private state、headless helper、reducer 或 `loop_step`。Display 与真实 Audio PAL 继续接入，只有 Main App 读取的 microphone frame 被 deterministic PCM 代替；真实 mic 仍以零等待方式采集并单独报告健康。详细合同见 产品 E2E。

## PAL

`h2_pal_e2e_run()` 要求 launcher 通过 `suite_mask` 显式选择 suite。`core` 与 MQTT 可以组合执行；Preference 必须单独选择，因为它会返回跨 boot action。MQTT suite 通过 Runtime 的 MQTT 与 monotonic Time API 执行 connect、subscribe、publish echo、disconnect 和 bounded cleanup。`host` suite 只通过注入的 Runtime/PAL API 运行相同 case ID 和结果 ledger；`//projects/e2e/targets/cc_binary/pal:pal_e2e_test` 以 OS-selected fixture 在 Linux、macOS 和 Windows 使用 ephemeral loopback port、临时 mount 与仓库内测试证书，不访问公网。Preference suite 只使用 `runtime->pref` 和 Memory PAL，在固定 control/data namespace 中执行 `seed -> verify -> clean -> complete`，覆盖全部类型、16 KiB blob、同值写、1,000 次替换、迭代、删除、清空和终态重放；跨 boot action 由结果返回，portable App 不直接重启平台。

Desktop launcher 位于 `projects/e2e/targets/cc_binary/pal`。MQTT public-broker target 从现有 `H2_MQTT_SMOKE_*` environment surface 读取 endpoint policy；loopback target 持有 POSIX broker fixture。`pref_test` 在 `TEST_TMPDIR` 下创建进程独占的 SQLite store，每阶段销毁并重新打开真实 provider，最后只删除自己的临时根。DevKit 与 Tiga adapter 分别位于 `projects/e2e/targets/h2loader_tar_zlib/pal-pref/devkit` 和 `projects/e2e/targets/h2loader_tar_zlib/pal-pref/tiga_esp_v4_2`；两者每次启动运行一个 Preference phase，只在 seed 成功后确认 App，并对两个 transition 使用真实重启；失败和终态都保持 H2Loader command-responsive。

Public MQTT broker 是 PAL 中唯一个 manual Bazel test，通过独立入口运行：

```sh
make bazel-test-mqtt_public_broker_smoke
```

Browser launcher 位于 `projects/e2e/targets/pkg_tar/pal`，只选择不需要外部服务的 `core` suite。它在一个 Web task 中运行 portable registry，逐条输出 bounded ledger；unsupported case 通过 normal wrapper 验证 canonical provider，不把缺失能力算作平台成功实现。

## H2Loader Serial

`h2_h2loader_serial_e2e_run()` 接收初始化后的 Runtime、独立注入的 Host Serial API、opaque port ID、预期 board/target、closed typed command，以及 install 所需的 catalog bytes、精确 asset SHA-256 和资源读取回调。portable App 拥有 preflight、authoritative status、安全只读 command、managed install/reconnect/final verification 和固定 ledger；它不读取文件、environment 或 DOM，也不选择 concrete provider。

macOS Desktop launcher 先通过 Darwin Serial 运行同一 App。Browser launcher 必须从直接用户手势取得 Web Serial 授权，再把 opaque ID 交给 App；scan 不打开 chooser。确定性 Node validation 只证明 Web Serial/PAL/Host Core wasm graph 与 preflight，不能替代真实设备的 status、HELP、install 或跨 OS Chrome evidence。无法通过稳定 USB identity 关联重启后原设备时必须失败，不得按 label 或 VID/PID 自动换设备。

## GizClaw

`h2_gizclaw_e2e_run()` 只消费调用方提供的 Runtime/PAL、endpoint、RegistrationToken、suite mask 与确定性 PCM。App 不读 environment 或文件，不选择 AP/BJ，不创建 Wi-Fi task，也不拥有 H2Peer/Pion。一个 case 失败后继续执行独立 case，最后输出完整 bounded summary 并完成反向清理。

`service` suite 在 portable GizClaw E2E App 内启动真实 GizClaw service worker，通过 app runner dispatch request callback。它使用 service-owned client 完成 Register 与 Ping，验证 progress、terminal completion、排队 request cancel，以及 stop、drain、deinit；不依赖 H106 App 或 LVGL subject。

Desktop live E2E 位于 `projects/e2e/targets/cc_test/gizclaw`，以两个独立的 `manual` test target 运行：`gizclaw_h2peer_live_test` 默认执行 H2Peer 的完整 suite，`gizclaw_pion_live_test` 默认执行 Pion 的 Firmware 与 Voice suite。它们默认使用自然入口 `ap`，workflow 可以通过 test environment 选择 `ap`/`bj` 和受 backend 支持的 suite；两个 target 从 test environment 继承真实 RegistrationToken。两个 Make 入口都直接运行对应 Bazel test：

```sh
make bazel-test-gizclaw_h2peer_live_test
make bazel-test-gizclaw_pion_live_test
```

DevKit launcher 位于 `projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit`，固定使用 H2Peer、北京入口和 RuntimeProfile `default` 自己的 `deploy-default` RegistrationToken。通用 RPC/Voice 测试从该 profile 返回的 `assistants` catalog 选择真实 Workflow，不假设 H106 的 `chat` alias。它在首次 Wi-Fi `GOT_IP` 后每次 boot 只运行一次 `all`；断线重连不创建第二个 runner。portable App 继续 non-fail-fast 执行全部独立 case，launcher 在完成后每 10 秒重放 bounded summary。Image confirmation 只证明 Runtime、H2Loader command service、Wi-Fi supervisor 和报告基础设施可用，不以业务 case 全部通过为条件。

真实 RegistrationToken 由 repository-approved test launcher 固定，或由 CI environment 注入。Token、private key、authorization metadata、Firmware URL、原始音频与 unrestricted response body 不得进入日志或 artifact。

## Libco

`h2_libco_smoke_run()` 在一个 `h2_libco_t` executor 中验证 FIFO、wait/wake、timeout、cancel、join、bounded cleanup 与 repeated switching。Public `h2_libco_smoke_*` symbol、`H2_LIBCO_SMOKE_*` marker、默认 8 KiB stack 和 10,000 次 switch 是跨 launcher 的稳定合同；BK3633 因 16 KiB Board Memory arena 显式使用 2 KiB stack。

同一份 portable source 由五类 launcher 执行：Desktop 在 process main thread 使用 upstream host backend；Browser root 使用 Emscripten Fiber/Asyncify backend；DevKit 在 pinned ESP-IDF main task 中使用 S3-only Xtensa backend；BK7258 在 Board entry AP task 中使用 repository-owned Cortex-M Thumb backend；BK3633 在 SDK boot/main context 中使用 upstream ARMv5 backend，以 target executor 的 BLE Stack task 推进 RWIP，但不进入 TapDoki production App。Cortex-M backend 除 AAPCS callee-saved registers 外，还在 Armv8-M 上保存 coroutine 对应的 `PSPLIM` 和 `PRIMASK`，避免 FreeRTOS task 的 hardware stack limit 被错误沿用到 heap-backed coroutine stack。Libco backend 不是 PAL capability 或 Runtime vtable；Browser platform 只在自己的 Web Task provider 内部复用它。

## Lua Runtime

`lua-runtime` 固定执行 `vm-source-load`、`coroutine-api`、
`coroutine-concurrency`、`timer-wakeup`、`component-lookup`、
`component-event`、`cancel-timeout-race`、`multi-vm-concurrency` 和
`shutdown-with-waiters`。App non-fail-fast 汇总九个
结果；每个 launcher 输出 `H2_LUA_E2E_CASE` 和最终 `H2_LUA_E2E` marker。

Desktop 和 AMOLED 配置两个 worker 并报告 `scheduler=multi-worker`；报告只说明
四个隔离 VM 分配到 Runtime worker，不用耗时推断 CPU parallelism。Browser 配置
一个 Web worker 并报告 `scheduler=cooperative`。Runtime Event queue 仍只由 App
消费；event case 使用一个 target-independent synthetic component，由 App 把复制
事件定向投递给显式 job。

Desktop catalog identity 是 `e2e/libco`，Bazel binary 是 `//projects/e2e/targets/cc_binary/libco:e2e-libco`。DevKit 与 BK7258 保留 H2Loader image/package identity `libco-smoke`。TapDoki BK3633 的 standalone full-image target 是 `//projects/e2e/targets/bk3633_firmware/libco-smoke/tapdoki_v2_0:firmware`；它保留 `tapdoki_libco_smoke` native target、merge identity 与 READY/FAIL evidence。

## WebRTC Performance

`h2_webrtc_performance_run()` 只消费调用方提供的 Runtime、profile、STUN URL 和 offer exchange callback。portable App 固定执行可比较的 app-shaped workload：三个 request DataChannel、Packet/Event 长连接、先下载 10 MiB 再上传 10 MiB，以及 20 ms Opus RTP 共载。它校验 exact byte count、payload、channel lifecycle 和音频 sequence，并输出逐轮 JSON 与 median；loaded/data-only throughput median 必须不低于 0.80。Desktop `benchmark` 要求每组 100 个 RTP frame 零丢包、零 duplicate、零 reorder、零 deadline miss、零发送侧 `WOULD_BLOCK`，到达间隔 p99 不超过 42 ms；设备 `smoke` 允许最多 5 个网络丢包和 200 ms p99，发送侧 deadline miss 与 `WOULD_BLOCK` 只记录诊断数据，duplicate 和 reorder 仍必须为零。partial transfer、timeout、错误 route 或不完整指标都失败关闭。

Desktop launcher 位于 `projects/e2e/targets/cc_binary/webrtc-performance`，自动启动并回收只绑定 loopback 的 Pion fixture。DevKit H2Loader App 位于 `projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/devkit`；operator 必须在构建时显式注入可从测试 Wi-Fi 访问的 HTTP signaling 和 STUN LAN endpoint，launcher 同时输出 internal、DMA-capable 与 PSRAM heap 的 KiB checkpoint。Desktop loopback 结果不能替代真实 DevKit、Wi-Fi 或远程服务证据。

## Validation Boundary

Portable/desktop tests 证明 case contract、provider assembly、parser、failure aggregation 与 cleanup。Live GizClaw 证明真实 E2E service flow。Firmware build 只证明对应 SDK graph 可以产生 image；DevKit/BK7258 必须继续通过 H2Loader-first install、confirm 与 cold-boot 验收，BK3633 必须按 Board guide 验证完整 `merge-crc.bin` 与两次 cold boot。任何一层不能代替另一层。
