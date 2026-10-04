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

可在 host 上确定性运行、没有外部依赖的 App-local、parser、fake 和 loopback 测试属于默认自动测试，不声明 tag。连接外部服务、真实设备或要求人工准备环境的 E2E test 声明 Bazel 特殊 tag `manual`，因此不会被通配 target pattern 自动选中；需要重新观察真实外部状态的 live test 另声明特殊 tag `external`，由 Bazel 禁用测试结果缓存，构建 disk/remote cache 保持启用。新的手工测试入口直接调用 `bazel test --config=<platform> --nocache_test_results <exact-label>`，设备选择与需要继承的 SDK 路径通过 `--test_env` 传入；平台配置和静态 suite 参数属于 `.bazelrc` / `BUILD.bazel`。不为纯转发另建 Make alias、shell wrapper 或 Python Bazel 调度器；已有 legacy Make 入口暂保留兼容，不再维护额外 tag filter。测试仅使用这些 Bazel 原生控制 tag，不另建业务 tag inventory，也不创建 `test_suite` 聚合入口。

## Apps

| App | Portable target | Current launcher matrix |
| --- | --- | --- |
| Atomic | `//projects/e2e/apps/atomic/app:atomic_e2e` | macOS、真实 Browser/WASM pthread Workers、iOS/Android 模拟器、DevKit ESP32-S3 与 BK7258 的完整 typed 接口资格 |
| GizClaw | `//projects/e2e/apps/gizclaw/app:gizclaw_e2e` | Desktop H2Peer/Pion；Chromium Worker；iOS XCFramework / Android AAR 消费 App；DevKit、AMOLED、BK7258 入口，实际资格见 App README |
| H106 | `//projects/e2e/apps/h106/app:h106_e2e` | Desktop Tiga/Zero、Tiga V4.2 与 Zero BK 1.0；完整 production Main App、Runtime Test Control 与公开 observation |
| Libco | `//projects/e2e/apps/libco/app:libco_smoke` | Desktop、Browser、DevKit ESP32-S3、BK7258、TapDoki BK3633 |
| Lua Runtime | `//projects/e2e/apps/lua-runtime/app:lua_runtime_e2e` | Desktop、Browser、AMOLED；九个固定 VM/coroutine/component/event/worker/shutdown case |
| Lua Link | `//projects/e2e/apps/lua-link/app:lua_link_e2e` | DevKit ESP32-S3 host + AMOLED ESP32-S3 join over BLE Extended Advertising；reliable/datagram/stream/peer-exit per session，final hold session for link loss |
| PAL Core | `//projects/e2e/apps/pal-core/app:pal_core_e2e` | 独立 Core v2：46 个接口、41 个必过用例；macOS、Browser/WASM、DevKit ESP32-S3 USB 串口、BK7258 AP UART1 H2Loader |
| PAL JSON | `//projects/e2e/apps/pal-json/app:pal_json_e2e` | 独立 JSON：24 个接口、15 个必过用例；macOS、Browser/WASM、iOS/Android 实际 SDK 包、DevKit 与 BK7258，记录完整运行与清理证据 |
| PAL HTTP | `//projects/e2e/apps/pal-http/app:pal_http_e2e` | 独立 HTTP：45 个必跑 case；macOS、Browser、iOS/Android 实际 SDK 包消费 App 与 DevKit/BK7258 专用入口，逐平台保留真实运行证据 |
| PAL MQTT | `//projects/e2e/apps/pal-mqtt/app:pal_mqtt_e2e` | 独立 MQTT：8 个操作、36 个必跑 case；真实 TCP/TLS broker、事件/ACK 与资源清理，按各平台实际执行记录验收 |
| PAL WebRTC | `//projects/e2e/apps/pal-webrtc/app:pal_webrtc_e2e` | 独立 WebRTC：13 个操作、43 个 mandatory case；六端独立入口及真实 Pion 对端，按各端完整 ledger 授予资格 |
| PAL Audio Decoder | `//projects/e2e/apps/pal-audio-decoder/app:pal_audio_decoder_e2e` | 独立 AAC-LC RAW 解码：8 个操作、29 个必过 case；六端入口已实现，实际资格以各端完整 PCM、生命周期与清理记录为准 |
| PAL Audio | `//projects/e2e/apps/pal-audio/app:pal_audio_e2e` | 独立 Audio：11 个 provider 与 5 个 track 操作、24 个必过 case；macOS、真实 Chromium Worker、iOS/Android SDK 包消费 App、AMOLED ESP32-S3 与 BK7258，逐端验证 30 秒同时采播和完整清理 |
| PAL Display | `//projects/e2e/apps/pal-display/app:pal_display_e2e` | 独立 Display：6 个操作、24 个必过 case；macOS SDL、Chromium Worker、iOS/Android 实际 SDK 包、AMOLED 与 BK7258，验证实际输出并单独记录物理屏幕观察 |
| PAL | `//projects/e2e/apps/pal/app:pal_e2e` | Linux/macOS/Windows 共同 host OS/Filesystem/Net/TLS/CoreHTTP/CoreMQTT；Desktop core/MQTT/SQLite Preference；Browser core；DevKit 与 Tiga V4.2 H2Loader `pal-pref` |
| H2Loader Serial | `//projects/e2e/apps/h2loader-serial/app:h2loader_serial_e2e` | macOS Desktop；desktop Chrome Browser |
| WebRTC Performance | `//projects/e2e/apps/webrtc-performance/app:webrtc_performance` | Desktop H2Peer + local Pion；DevKit 与 AMOLED ESP32-S3 H2Peer + operator LAN Pion |
| iperf | `//projects/e2e/apps/iperf/app:iperf_e2e` | Desktop host client + PAL server；AMOLED ESP32-S3 + operator LAN PAL server |

独立 HTTP 与 MQTT 测试分别由 `pal-http` 和 `pal-mqtt` App 持有公共 case 和平台验收合同。PAL App 只验证 PAL API 的跨目标公共行为，不吸收 backend-local unit、fake 或 protocol tests。Provider 名属于 launcher target；不能为了 H2Peer、Pion 或另一 backend 复制 portable case registry。H106 production App、adapter、UI 与业务 policy 继续属于 `projects/h106`；H106 E2E 的 evidence boundary 和运行合同见 产品 E2E。

## Atomic

Atomic 是独立 library，不属于 PAL。`projects/e2e/apps/atomic/app` 的固定 28-case registry 覆盖九种 typed wrapper 的全部 76 个 typed function、静态/动态生命周期、有效 memory order、C generic dispatch、争用、flag exclusion 与 release/acquire publication。每轮实际启动并 join 10 个 task；所有失败、timeout、未执行或 cleanup error 都不能资格 PASS。`api_coverage.json` 保存接口到 case 的映射。

六端 entry 都直接使用 Bazel：Desktop 的 `targets/cc_test/atomic:atomic_e2e_test`；真实 Chromium 的 `targets/pkg_tar/atomic:atomic_browser_test`；iOS/Android 的 `targets/ios_application/atomic:ios_atomic_simulator_test` 与 `targets/android_binary/atomic:android_atomic_simulator_test`；两块板的 `targets/h2loader_tar_zlib/atomic/devkit:device_test` 与 `targets/h2loader_tar_zlib/atomic/bk7258_v3_202405:device_test`，以上 prefix 均为 `//projects/e2e/`。Web C 在真实 pthread Workers 使用 shared Wasm memory，验收须观察不同 Worker identity；Node `atomic_wasm_test` 是额外对照。移动端消费真实 XCFramework/AAR 并复用共享 mobile runner，精确检查 28/28、10/10 joins、resource balance 与 provider shutdown；模拟器结果不能冒充实体手机证据。

DevKit 与 BK7258 分别在内部 RAM 和 PSRAM wrapper placement 运行整套资格，每次 boot 要求 56/56 与 20/20 joins。两平台的实际 atomic backing 都必须在内部 RAM，必须观察 CPU0/CPU1 与 live object/allocation 清理；不能从 task policy 名推断实际 core。DevKit 另检查两枚地址独立的 file-static flag、PSRAM 动态 wrapper/internal backing，以及 CPU0 上优先级 4/9 的两名 worker 在 ready/go barrier 后各执行 20,000 次操作。直接 C11 只在受支持的内部 RAM 做独立对照；Xtensa PSRAM backing 不受支持，不执行该实验，也不把它混入 H2Atomic 必测资格。

旧 DevKit UID `9888e0115c52` 的记录中，六轮 H2Atomic counter 达到目标，但三轮 direct-C11 PSRAM 对照丢 count，App 仍无条件 confirm。这是历史实验，不能当完整接口验收。当前 launcher 只有全部必测与 cleanup 成功才确认 App；direct device test 必须核对显式 port/UID、原 P1 与空 Stage（只允许继续完全相同的既有 package）；suite 与严格 PAL resource comparison 后才启动串口 command service。实际 cleanup 和 confirm 成功后，才重放包含每次真正 boot 独立 crypto execution identity 的不可变 ledger；managed version 必须不同于当前 App；排除 CLI 接受 reboot 前的所有 replay，升级与独立 App reboot 必须观察不同 identity 下完整的 56-case ledger。最终回读 package/version/image、原 P1 与空 Stage；已有 coredump 实际读取并逐字节比较，空 dump 只校验原 blank status，不伪造内容或 digest。Live target 声明 `manual`/`external`，artifact build cache 保持启用。完整运行命令和 source/artifact 证据身份见 `projects/e2e/apps/atomic/README.md` 与生成的资格收据。

## H106

`projects/e2e/apps/h106/app` 持有跨目标 case registry、bounded terminal ledger、non-fail-fast aggregation、Audio decorator、Main App supervisor 和报告合同。H106 产品组继续拥有 production Main App、产品 policy 与 Desktop/Tiga/Zero artifact entry；各 launcher 只提供 production Runtime/provider assembly、各产品自己的 checked-in RegistrationToken、固定 AP E2E endpoint、目标 memory reader 和 H2Loader lifecycle。

每个目标都启动完整阻塞式 `h2_h106_run()`；测试只通过 Runtime Test Control 注入 public component event，并且只读取公开 bounded observation。H106 E2E 不调用 private state、headless helper、reducer 或 `loop_step`。Display 与真实 Audio PAL 继续接入，只有 Main App 读取的 microphone frame 被 deterministic PCM 代替；真实 mic 仍以零等待方式采集并单独报告健康。详细合同见 产品 E2E。

## PAL

`h2_pal_e2e_run()` 要求 launcher 通过 `suite_mask` 显式选择 suite。`core` 与 MQTT 可以组合执行；Preference 必须单独选择，因为它会返回跨 boot action。Wi-Fi suite 必须单独选择：断开 STA，需要非 Wi-Fi 控制链路，不恢复连接，只验证断开后 STA/Netif 状态一致；`filesystem` suite 只运行 HOST_FILESYSTEM case。worker join 或 timer destroy 失败时资源保留在 `retained_cleanup`，后续 suite（含 MQTT）不再运行，直到 `h2_pal_e2e_cleanup()` 成功。MQTT suite 通过 Runtime 的 MQTT 与 monotonic Time API 执行 connect、subscribe、publish echo、disconnect 和 bounded cleanup。`host` suite 只通过注入的 Runtime/PAL API 运行相同 case ID 和结果 ledger；`//projects/e2e/targets/cc_binary/pal:pal_e2e_test` 以 OS-selected fixture 在 Linux、macOS 和 Windows 使用 ephemeral loopback port、临时 mount 与仓库内测试证书，不访问公网。Preference suite 只使用 `runtime->pref` 和 Memory PAL，在固定 control/data namespace 中执行 `seed -> verify -> clean -> complete`，覆盖全部类型、16 KiB blob、同值写、1,000 次替换、迭代、删除、清空和终态重放；跨 boot action 由结果返回，portable App 不直接重启平台。

Desktop launcher 位于 `projects/e2e/targets/cc_binary/pal`。MQTT public-broker target 从现有 `H2_MQTT_SMOKE_*` environment surface 读取 endpoint policy；loopback target 持有 POSIX broker fixture。`pref_test` 在 `TEST_TMPDIR` 下创建进程独占的 SQLite store，每阶段销毁并重新打开真实 provider，最后只删除自己的临时根。DevKit 与 Tiga adapter 分别位于 `projects/e2e/targets/h2loader_tar_zlib/pal-pref/devkit` 和 `projects/e2e/targets/h2loader_tar_zlib/pal-pref/tiga_esp_v4_2`；两者每次启动运行一个 Preference phase，只在 seed 成功后确认 App，并对两个 transition 使用真实重启；失败和终态都保持 H2Loader command-responsive。

Public MQTT broker 是 PAL 中唯一个 manual Bazel test，通过独立入口运行：

```sh
make bazel-test-mqtt_public_broker_smoke
```

Browser launcher 位于 `projects/e2e/targets/pkg_tar/pal`，只选择不需要外部服务的 `core` suite。它在一个 Web task 中运行 portable registry，逐条输出 bounded ledger。Core 不再假设 Filesystem 必须返回 UNSUPPORTED；缺失能力不能算作平台成功实现。

JieLi AC791N 的 `projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit` 复用公共 Core 与独立 Wi-Fi suite，通过共享 H2Loader layout 和 UART1 运行。Wi-Fi 当前覆盖断开后的 STA/Netif 状态一致性，不代表扫描、连接、AP 和 Runtime Event 已验收；测试 App 保持未确认，以便异常复位返回 Loader。

## H2Loader Serial

`h2_h2loader_serial_e2e_run()` 接收初始化后的 Runtime、独立注入的 Host Serial API、opaque port ID、预期 board/target、closed typed command，以及 install 所需的 catalog bytes、精确 asset SHA-256 和资源读取回调。portable App 拥有 preflight、authoritative status、安全只读 command、managed install/reconnect/final verification 和固定 ledger；它不读取文件、environment 或 DOM，也不选择 concrete provider。

macOS Desktop launcher 先通过 Darwin Serial 运行同一 App。Browser launcher 必须从直接用户手势取得 Web Serial 授权，再把 opaque ID 交给 App；scan 不打开 chooser。确定性 Node validation 只证明 Web Serial/PAL/Host Core wasm graph 与 preflight，不能替代真实设备的 status、HELP、install 或跨 OS Chrome evidence。无法通过稳定 USB identity 关联重启后原设备时必须失败，不得按 label 或 VID/PID 自动换设备。

## Lua Link

`h2_lua_link_e2e_run()` 在已启动的 BLE Host 上创建一个 Lua Host，调用 `h2_lua_link_enable()` 并以 `args.role`（`host` 或 `join`）运行固定脚本。每个 session 依次测 20 次可靠消息往返、双向各 200 条有序可靠消息、双向各 100 条 50 Hz datagram、双向各 64 KiB 逐字节校验的字节流，最后 joiner 离开、host 必须报告 `peer_closed`；结果以 `LINK stage=...` 和 `H2_LUA_LINK_E2E result=...` 写入 Runtime Log。`hold` 模式保持一个 10 Hz datagram session 直到链路断开，并报告断开距最后一条 datagram 的时间。App 只依赖 Runtime 与 `link` module，不使用 Wi-Fi；`//projects/e2e/apps/lua-link/app:lua_link_e2e_test` 在 fake BLE air 上让两个角色互跑完整 suite 和 hold 断链。

DevKit launcher（`projects/e2e/targets/h2loader_tar_zlib/lua-link/devkit`）运行 `host`，AMOLED launcher（`.../lua-link/amoled`）运行 `join`，两者都使用 Extended Advertising/Scanning。H2Loader App command service 启动 BLE Host；image 在 command service 运行后确认，只证明基础设施可用。每次 boot 连续运行五个 session 并输出 `stage=summary ... passed=<n> rounds=5`，最后进入 `hold` session。先启动 AMOLED joiner，再启动 DevKit host。

## GizClaw

GizClaw live入口统一直接 `bazel test`；iOS/Android消费共享mobile runner，业务fixture/oracle在`gizclaw-mobile/suite.py`，不再使用Make或shell转发调Bazel。`gizclaw_e2e_fixture` macro生成显式profile/key/已知非敏感value合同，缺输入在注册与业务资源mutation前失败。AppConfig两套list必须包含所选key，两套get必须逐字节等于期望value；注册返回必须匹配所选E2E profile，不能把其他profile的配置算进default资格。旧default/226项记录保留身份，新主线公开inventory为227项。

`Live E2E` workflow 的 GizClaw scope 显式接收 endpoint、RuntimeProfile、非敏感 AppConfig key/value；all suite 还需 Device API 和 audio HTTPS URL。RegistrationToken 只从独立 Secret 经环境变量注入；输入不插入可执行脚本。Workflow 直接请求 Bazel live labels，保留启用的 disk cache，并在 both backend 下保留首个失败结果且继续第二项。缺失受控输入在任何 live target 启动前失败。

`h2_gizclaw_e2e_run()` 只消费调用方提供的 Runtime/PAL、endpoint、RegistrationToken、suite mask 与确定性 PCM。防止并发 suite 和 retained session 被重复使用的 `s_run_active` 是文件级 static flag，使用 `H2_ATOMIC_DEFINE_STATIC` 定义独立 backing，不需模块级初始化或分配；run 结束且资源全部清理时清除，retained 资源仍在时保持占用。App 不读 environment 或文件，不选择 AP/BJ，不创建 Wi-Fi task，也不拥有 H2Peer/Pion。一个 case 失败后继续执行独立 case，最后输出完整 bounded summary 并完成反向清理。

Desktop C++ launcher 的进程级 run guard 由同 package 的 C 桥接文件定义普通 static backing，C++ 通过 typed accessor 借用 wrapper 后仍调用同一 `h2_atomic_flag_*` API；没有 launcher 专用 global init，也不假设 `std::atomic` 与 C11 `_Atomic` 的内存布局相同。

`service` suite 在 portable GizClaw E2E App 内启动真实 GizClaw service worker，通过 app runner dispatch request callback。它使用 service-owned client 完成 Register 与 Ping，验证 progress、terminal completion、排队 request cancel，以及 stop、drain、deinit；不依赖 H106 App 或 LVGL subject。

Desktop live E2E 位于 `projects/e2e/targets/cc_test/gizclaw`，以两个独立的 `manual` test target 运行：`gizclaw_h2peer_live_test` 默认执行 H2Peer 的完整 suite，`gizclaw_pion_live_test` 默认执行 Pion 的 Firmware 与 Voice suite。它们默认使用自然入口 `ap`，workflow 可以通过 test environment 选择 `ap`/`bj` 和受 backend 支持的 suite；两个 target 从 test environment 继承真实 RegistrationToken。两个 Make 入口都直接运行对应 Bazel test：

```sh
bazel test --config=macos_arm64 //projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_live_test --test_arg=--endpoint=<e2e-host:port>
bazel test --config=macos_arm64 //projects/e2e/targets/cc_test/gizclaw:gizclaw_pion_live_test --test_arg=--endpoint=<e2e-host:port>
```

DevKit launcher 位于 `projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/devkit`，使用 H2Peer 与明确注入的受控 E2E endpoint、RuntimeProfile、key/value、token 和 HTTPS fixtures；旧 default/deploy-default 运行记录保留原身份。通用 RPC/Voice 测试从该 profile 返回的 `assistants` catalog 选择真实 Workflow，不假设 H106 的 `chat` alias。它在首次 Wi-Fi `GOT_IP` 后每次 boot 只运行一次 `all`；构建时设置 `--define=H2_GIZCLAW_E2E_VOICE_ONLY=1` 可只运行 `voice`，用于隔离跨 case 的资源状态。断线重连不创建第二个 runner。portable App 继续 non-fail-fast 执行选中的独立 case，launcher 在完成后每 10 秒重放 bounded summary。普通 GizClaw E2E 仅在本轮选定用例全部通过、结果完整且没有清理失败/保留资源时确认 App。失败镜像保持未确认；AMOLED 显式 OTA-only 的源镜像准备仍遵循独立硬件升级流程。

DevKit 的 E2E runner、launcher 和 job task 显式使用 PSRAM stack；runner 入口以实际栈局部地址检查 PSRAM，失败时报告 harness error。`$gizclaw/net` 也使用 PSRAM，DevKit E2E policy 为它保留 64 KiB：在同一块 DevKit（UID `9888e0115c52`）上，原 32 KiB 实际栈曾在 Voice 的 `session_audio_start` 后溢出并重启。64 KiB 复测越过了该溢出点，`all` 的两轮实机测试分别为 8 项中 4 项通过、4 项失败：第一轮 `cleanup_rc=0`、`retained_resources=0`，第二轮出现 `peer_create_data_channel rc=-13`，`cleanup_rc=-4`、`retained_resources=8`。独立的 `voice` 实机测试中，PTT、文本、实时 VAD、service 重连和清理均通过，`selected=1`、`pass=1`、`cleanup_rc=0`、`retained_resources=0`。DevKit 无音频后端，其测试不作为真实麦克风和扬声器验收。测试没有取得 `$gizclaw/net` 的 stack high-water 数据，因此 64 KiB 不能作为其他固件 target 的容量结论。

AMOLED GizClaw E2E 使用板载 ES8311 的真实音频 delegate，并以局部栈地址检查 runner 确实在 PSRAM；该目标的 `$gizclaw/net` 测试 policy 同样为 64 KiB PSRAM。初次实机测试在 voice-replace 的 `peer_create_data_channel` 返回 `-13`，随后清理也失败；定位发现 `libs/app_test` 的 WebRTC 观察包装器每个 peer 固定保留 16 个 channel 句柄直到 peer 关闭，第 17 个句柄被包装器拒绝，底层 H2Peer 并未返回该错误。包装器改为按需分配稳定句柄后，UID `30eda0ae0f86` 上的完整 `voice` 测试通过：PTT、文本、history-play、实时 VAD、voice-replace、重连及清理均通过，真实音频 delegate 报告 `mic_starts=1`、`capture_frames=446`、`capture_first_error=0`、`capture_last_error=0`；最终 `selected=1`、`pass=1`、`cleanup_rc=0`、`retained_resources=0`。这只说明本轮创建的资源已清理；先前失败轮次的远端残留没有可用于安全定点删除的 ID。64 KiB 仅为此 E2E 目标的测试预算，尚无 stack high-water 证据支持推广到产品目标。

真实 RegistrationToken 由 repository-approved test launcher 固定，或由 CI environment 注入。Token、private key、authorization metadata、Firmware URL、原始音频与 unrestricted response body 不得进入日志或 artifact。

## Libco

`h2_libco_smoke_run()` 在一个 `h2_libco_t` executor 中验证 FIFO、wait/wake、timeout、cancel、join、bounded cleanup 与 repeated switching。Public `h2_libco_smoke_*` symbol、`H2_LIBCO_SMOKE_*` marker、默认 8 KiB stack 和 10,000 次 switch 是跨 launcher 的稳定合同；BK3633 因 16 KiB Board Memory arena 显式使用 2 KiB stack。

同一份 portable source 由五类 launcher 执行：Desktop 在 process main thread 使用 upstream host backend；Browser 的 C main Worker 使用 Emscripten Fiber/Asyncify backend；DevKit 在 pinned ESP-IDF main task 中使用 S3-only Xtensa backend；BK7258 在 Board entry AP task 中使用 repository-owned Cortex-M Thumb backend；BK3633 在 SDK boot/main context 中使用 upstream ARMv5 backend，以 target executor 的 BLE Stack task 推进 RWIP，但不进入 TapDoki production App。Cortex-M backend 除 AAPCS callee-saved registers 外，还在 Armv8-M 上保存 coroutine 对应的 `PSPLIM` 和 `PRIMASK`，避免 FreeRTOS task 的 hardware stack limit 被错误沿用到 heap-backed coroutine stack。Libco backend 不是 PAL capability 或 Runtime vtable；独立 libco App 验证它自身的协程行为，Browser PAL Task 则使用 pthread Workers，不再依赖 libco。

## Lua Runtime

`lua-runtime` 固定执行 `vm-source-load`、`coroutine-api`、
`coroutine-concurrency`、`timer-wakeup`、`component-lookup`、
`component-event`、`cancel-timeout-race`、`multi-vm-concurrency` 和
`shutdown-with-waiters`。App non-fail-fast 汇总九个
结果；每个 launcher 输出 `H2_LUA_E2E_CASE` 和最终 `H2_LUA_E2E` marker。

Desktop 和 AMOLED 配置两个 worker 并报告 `scheduler=multi-worker`；报告只说明 四个隔离 VM 分配到 Runtime worker，不用耗时推断 CPU parallelism。Browser 配置 两个 pthread Worker 并报告 `scheduler=multi-worker`。Runtime Event queue 仍只由 App 消费；event case 使用一个 target-independent synthetic component，由 App 把复制 事件定向投递给显式 job。

Desktop catalog identity 是 `e2e/libco`，Bazel binary 是 `//projects/e2e/targets/cc_binary/libco:e2e-libco`。DevKit 与 BK7258 保留 H2Loader image/package identity `libco-smoke`。TapDoki BK3633 的 standalone full-image target 是 `//projects/e2e/targets/bk3633_firmware/libco-smoke/tapdoki_v2_0:firmware`；它保留 `tapdoki_libco_smoke` native target、merge identity 与 READY/FAIL evidence。

## PAL WebRTC

`projects/e2e/apps/pal-webrtc/app` 是独立接口资格 App，消费 Runtime 和 launcher 提供的 Pion signaling callback。稳定 registry 覆盖 13 个 WebRTC 操作、Track read/write、owned event release、自定义 allocator 和 DataChannel config；完整结果要求所有 mandatory case PASS，缺接口、BLOCKED、SKIP 或仅构建均不算通过。真实 Pion 对端验证 ICE/DTLS/SRTP/SCTP、DataChannel、Opus、背压、关闭及错误 fingerprint 拒绝。合成 Opus packet 只验证协议传输，不宣称真实音频硬件；当前 PAL 没有视频接口。

`fingerprint-rejected` 要求有效 SDP 中的错误 digest 得到具体认证拒绝。PAL 的 `TLS_VERIFY` 是直接证据；浏览器未提供 RTCError 细节时，由同一 Pion session 捕获 typed received fatal `BadCertificate(42)` / `CertificateUnknown(46)` 且该 session 从未打开通道，独立确认认证原因。通用握手错误、连接超时、错误字符串、无效 SDP 都不能计 PASS；Desktop 另有真实 Pion 负回归验证后两种情况。显式 Stream ID 用双方 `negotiated=1` 的同 ID 通道验收，自动带内协商的四种 ordered/reliable 组合继续独立覆盖。

Desktop 和真实 Chromium 有自动测试；移动端通过实际 XCFramework/AAR package consumer 执行，入口为 `make bazel-test-ios_pal_webrtc_simulator_test` 和 `make bazel-test-android_pal_webrtc_simulator_test`。设备 launcher 位于 `targets/h2loader_tar_zlib/pal-webrtc`，借用已保存 STA 配置、读取显式 fixture 构建参数，运行一次后重放不可变 boot ledger。每个平台以自己 artifact 对应的完整结果资格为准，不从另一平台结果推断可用性。BK7258 的测试 allocator 和 H2Peer task stack 使用已有 PSRAM region，命令 Runtime 保持板级 allocator；失败 gate 不 confirm App。DevKit 先在 BSP 的 64 KiB 长期入口 task 初始化 H2Peer、Board Runtime 与命令服务，再启动独立测试 runner。

BK7258 在完整 43 项后额外建立一个真实 Pion 连接，持续 600 秒每秒交换带序号的二进制消息与合成 Opus。RTP 单包丢失时继续发送后续序号，分别记录唯一回传、missing、duplicate 和最长无有效媒体的间隔；至少 500 次 Data 与 Opus 成功回传，连续 10 秒无媒体恢复、Data 中断或 payload 错误均失败。真实 Pion 单包丢失恢复和持续丢媒体负例验证这项边界；Pion 同 session RX/TX/drop/gap counter 保留对端证据。独立 `H2_PAL_WEBRTC_SOAK` ledger 记录 run ID、单调 uptime、回传计数、耗时与结果，完成后关闭连接并检查 allocator 回收。启动输出 SDK reset reason。设备资格要求新镜像安装后和正常 App reboot 后各自通过，保存原 P1 Loader、Stage 空和 coredump 基线。旧镜像的 idle 观察仅说明该窗口内未复现 AP 重启，不能代替新镜像的活动稳定性验收，也不能据此排除 CP-only reset 或断言声音来源。

## PAL Audio

`projects/e2e/apps/pal-audio/app` 直接借用 launcher 提供的真实 Audio PAL 和 Time PAL，固定运行 24 个 mandatory case，覆盖全部 11 个 provider 与 5 个 track 操作。测试检查参数与 PCM format 边界、录音和播放、停止后读取、音量与麦克风增益、track 写入/drain/close、重复启停、至少 30 秒同时采播及唯一的终态 cleanup。缺失能力记为 BLOCKED，不能授予资格；`cleanup_once_test` 防止 terminal cleanup 后再次执行未报告的硬件恢复操作。接口映射与 compiler 实际代码覆盖率分别记录，不从 target 构建成功推断执行覆盖。

Desktop 验证真实 PortAudio callback；Browser C 在 pthread Worker 中通过 production getUserMedia/AudioWorklet provider 运行，输入使用 Chromium deterministic fake microphone。iOS Simulator 和 Android Emulator 分别消费实际 Swift Package/XCFramework 与 AAR；移动端 manual 入口为 `make bazel-test-ios_pal_audio_simulator_test` 和 `make bazel-test-android_pal_audio_simulator_test`。设备使用有真实音频 codec 的 AMOLED ESP32-S3 和 BK7258，要求托管升级与独立正常 App reboot 各自完成 ledger、成功 confirmation、原 P1 Loader 保持、Stage 空且 coredump 不变。

两个移动端另通过注入 tracked per-track allocator 验证 allocation failure 被正确拒绝、单帧 PCM write/drain/close 成功，以及全部 caller-owned 存储回收；Android provider 在 native output startup 前执行静音 priming，不能要求测试 App 写满启动 buffer 来掩盖单帧 drain 失败。桌面 manual 入口为 `make bazel-test-desktop_test`。

报告的 microphone PCM 计数/peak/energy 与输出 PCM frame/peak 说明真实 provider 数据路径已执行；write/drain 返回成功不能代替外部声学测量。Simulator 结果不代表物理手机，Browser fake microphone 不代表物理麦克风。`h2_pal_audio_decoder.h` 是独立 capability，需要另一个 session/packet 生命周期 E2E，不属于此 Audio 资格。

## PAL Audio Decoder

`projects/e2e/apps/pal-audio-decoder/app` 借用真实 Decoder/Memory/Time/Sync PAL，执行 29 个 mandatory case，覆盖全部 8 个操作。原始 AAC-LC RAW fixture 验证单/双声道实际 PCM 格式、频谱、声道分离、时间戳、借用输入、持有 frame、reset/reconfigure、EOS 和 allocation failure 后完整释放。缺失能力、失败或资源残留均不授予资格；这份资格独立于录音/播放 Audio PAL。

macOS 使用 FFmpeg；WASM C 在 Worker 执行、WebCodecs UI callback 只做有界私有 staging，调用方 PCM allocator 在 Worker acquire 时使用。要求 operator 明确选择具备 AAC codec 的 browser runtime；无 AAC 的开源 Chromium 记为 BLOCKED。三个环境相关入口为 `make bazel-test-wasm_pal_audio_decoder_browser_test`、`make bazel-test-ios_pal_audio_decoder_simulator_test` 与 `make bazel-test-android_pal_audio_decoder_simulator_test`。iOS/Android 消费实际 XCFramework/AAR；DevKit 和 BK7258 使用现有 native AAC decoder。设备只有全通过才 confirm，managed install 与独立正常 reboot 必须分别验收，保存原 P1、Stage 和 coredump 证据。当前六端资格仍在收集，不从构建成功推断通过；详细执行边界见 App README。

## PAL Display

`projects/e2e/apps/pal-display/app` 借用真实 Runtime 和 launcher 的输出观察器，覆盖 open/info/draw/present/brightness/close，固定运行 24 个 mandatory case。测试检查 native 格式、borrowed 像素生命周期、全屏及局部更新、padding/unaligned stride、可选格式的明确支持或拒绝、矩形/整数溢出、重复 present、0/50/100 亮度和重复 close/reopen；失败、未运行或清理失败不能授予资格。

Desktop 从 SDL renderer 读回，Browser 比对真实 Chromium composited screenshot，移动端捕获已显示的 UIKit/Android View。移动端分别消费实际 Swift Package/XCFramework 和 AAR，manual 入口为 `make bazel-test-ios_pal_display_simulator_test` 与 `make bazel-test-android_pal_display_simulator_test`。

AMOLED 比对已完成 SPI DMA 的传输数据；BK 比对 LCD controller 当前 DMA source 并检查 refresh counter。两者均不代表光学像素或实际亮度测量。物理图案及稳定性需要相机/readback fixture 或用户明确观察确认；亮度控制使用真实控制器命令/PWM 更新和 mandatory ledger 验证，人工逐档观察及光度测量单独记录。设备必须在 managed 安装启动和独立正常 reboot 后各自完成测试，保留原 Loader/P1 与 coredump，最终 Stage 空且 App confirmed。具体能力 profile 和证据边界见 [PAL Display App](https://github.com/GizClaw/gizos/tree/main/projects/e2e/apps/pal-display)。

## WebRTC Performance

`h2_webrtc_performance_run()` 只消费调用方提供的 Runtime、profile、STUN URL 和 offer exchange callback。portable App 固定执行可比较的 app-shaped workload：三个 request DataChannel、Packet/Event 长连接、先下载 10 MiB 再上传 10 MiB，以及 20 ms Opus RTP 共载。它校验 exact byte count、payload、channel lifecycle 和音频 sequence，并输出逐轮 JSON 与 median；loaded/data-only throughput median 必须不低于 0.80。Desktop `benchmark` 要求每组 100 个 RTP frame 零丢包、零 duplicate、零 reorder、零 deadline miss、零发送侧 `WOULD_BLOCK`，到达间隔 p99 不超过 42 ms；设备 `smoke` 允许最多 5 个网络丢包和 200 ms p99，发送侧 deadline miss 与 `WOULD_BLOCK` 只记录诊断数据，duplicate 和 reorder 仍必须为零。partial transfer、timeout、错误 route 或不完整指标都失败关闭。

Desktop launcher 位于 `projects/e2e/targets/cc_binary/webrtc-performance`，自动启动并回收只绑定 loopback 的 Pion fixture。DevKit H2Loader App 位于 `projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/devkit`；operator 必须在构建时显式注入可从测试 Wi-Fi 访问的 HTTP signaling 和 STUN LAN endpoint，launcher 同时输出 internal、DMA-capable 与 PSRAM heap 的 KiB checkpoint。Desktop loopback 结果不能替代真实 DevKit、Wi-Fi 或远程服务证据。

## iperf

`h2_iperf_e2e_run()` 只消费 launcher 借用的 Memory、Net、Time、Crypto、可选 SCTP 与 Log PAL，以及 operator 提供的 server 地址。portable App 拥有固定顺序的 case 矩阵（TCP 参考、UDP 上下行 10/20/40 Mbit/s 与 1200 B datagram、SCTP 上下行 1200 B 与 1400 B packet budget）、每个 case 的 deadline、non-fail-fast aggregation、每 case 一行 `H2_IPERF_E2E_CASE` JSON 和 `H2_IPERF_E2E_SUMMARY`。它不读取 environment 或文件，不连接 Wi-Fi，也不选择 provider；SCTP case 在没有 SCTP PAL 时记为 `UNSUPPORTED` 并继续。

App-local `iperf_e2e_test` 用 `//libs/iperf:test_support` 在 loopback 上对同一个 PAL server 顺序执行 TCP、UDP reverse 与两条 SCTP association，验证共享封装 socket 上的后续 association 不会被前一条的尾包污染。host launcher `//projects/e2e/targets/cc_binary/iperf:h2iperf` 同时提供 `server`（TCP/UDP/SCTP-over-UDP 的 PAL server）和 `client <host>`（同一矩阵）。AMOLED launcher 位于 `projects/e2e/targets/h2loader_tar_zlib/iperf/amoled`，从 Runtime `wifi_settings` 取回 Loader 保存的 STA 配置，用 `--define=H2_IPERF_SERVER` 指定 LAN server，并用 `--define=H2_IPERF_POWER_SAVE` 选择 Wi-Fi 省电策略。

## Validation Boundary

`projects/e2e/apps/pal-wifi` independently maps all 21 STA/AP/Settings/Netif
operations to 38 portable cases and one separate-boot persistence case.
DevKit/BK7258 qualification needs real WLAN scan/authentication/DHCP and an
AMOLED AP/STA fixture that joins and leaves each DUT AP. The launcher makes a
private crash-recovery credential backup before mutations; restores original
Settings and network; removes the backup; and only confirms a complete qualified
boot. Without saved STA credentials, the portable suite rejects an active or
indeterminate initial connection before mutation, and verifies disconnected
status again during restoration. A first seed boot is pending persistence and is not confirmed; ESP's
unconfirmed-image rollback means that seed can precede a different managed
image under the fixed v1 record contract. Receipts keep each actual version;
the final confirmed artifact also passes an ordinary App reboot. macOS uses
native Netif read-only; WASM tests HOST Netif offline/online Runtime delivery;
iOS/Android default SDK assembly explicitly reports unsupported. A host capability
contract PASS is not physical WLAN qualification. Wi-Fi CSI is separate.
The AP+STA route case uses an independently saved infrastructure AP when present,
retaining real addressing and explicit AP→STA route/event assertions. Standalone
open/hidden AP modes disconnect that upstream STA before using their requested
channel. BK uses paired AP/CP actual association/disassociation commands and SDK
MIN/MAX listen-interval readback; these do not claim measured radio power.
The source/package/status/coredump audit is separate from portable historical
receipt consistency. Entrypoints and evidence live with the independent App README.
The final DevKit R35 and BK7258 R34 images each passed 39/39 on both managed install
and a separate ordinary App boot, with six fresh fixture clients per qualified
boot, complete Settings/network restoration, empty Stage and unchanged P1 and
coredump baselines. macOS, WASM, iOS Simulator and Android Emulator each passed
21/21 applicable capability checks; they do not qualify physical WLAN.

Portable/desktop tests 证明 case contract、provider assembly、parser、failure aggregation 与 cleanup。Live GizClaw 证明真实 E2E service flow。Firmware build 只证明对应 SDK graph 可以产生 image；DevKit/BK7258 必须继续通过 H2Loader-first install、confirm 与 cold-boot 验收，BK3633 必须按 Board guide 验证完整 `merge-crc.bin` 与两次 cold boot。任何一层不能代替另一层。

## iOS / Android PAL Core

移动端 Core 资格测试复用 `projects/e2e/apps/pal-core` 的全部 41 个必选 case， 由两个原生 App 运行：`projects/e2e/targets/ios_application/pal-core` 和 `projects/e2e/targets/android_binary/pal-core`。它们消费 PAL provider 的本地 XCFramework/Swift Package、AAR 产物；Runtime 与 Atomic 仍由 App host 单独组装。 任务栈、资源计数、日志及系统事件均观察真实实现，不能用空 observer 或 blocked 替代通过。

设置 `H2_IOS_SIMULATOR_UDID` 或 `H2_ANDROID_SERIAL` 后，使用 `make bazel-test-ios_pal_core_simulator_test` / `make bazel-test-android_pal_core_simulator_test`。 这些入口要求已准备好的测试模拟器，属于 `manual`，每次真实执行并检查完整 case 清单、 清理状态和资源恢复。详情见 `projects/e2e/libs/pal-core-mobile/README.md`。

## PAL HTTP

`pal-http` 独立验收 HTTP request/response_free 和所有 request 字段，包括七种方法、字节 span、三种响应内存模式、流式 byte count、header/read callback 错误传播、取消、整体 deadline/retry、重定向、证书验证与资源释放。App 只借用 Runtime/PAL；`projects/e2e/libs/pal-http-fixture` 提供隔离 session 的可重复 HTTP/HTTPS 服务，host/browser 默认只绑定 loopback。Browser 使用真实 Fetch/CORS，只信任该次 fixture 的指定 SPKI，仍必须拒绝独立的不受信任证书。不存在的网卡必须显式报错；Browser 的绑定拒绝不代表支持物理网卡选择。设备入口只借用已有 Wi-Fi 配置和 Board Net provider，不更改 provisioning。每端必须完整运行同一 registry 并保留对应 artifact SHA 的结构化 receipt，构建成功不能替代实测。 不受信任 HTTPS 必须关联本轮真实 TLS 握手（ClientHello、服务端证书、拒绝且无成功 HTTP）；native 要求 TLS_VERIFY，Browser 还需 exact URL 的 Chromium ERR_CERT_AUTHORITY_INVALID。Browser teardown 后由运行 C 的 pthread 通过生产 bridge 读取实际 HTTP registry 并报告零 pending；缺失或非法计数必须失败，不能从页面全局 Module 猜测。坏 URL、拒绝连接和缺失 registry 均有真实负向运行。

## PAL Storage

`projects/e2e/apps/pal-storage` 是独立 portable App，覆盖 FileSystem 的 11 个 vtable 操作，以及 Preferences API 的 open 和 namespace 的 16 个方法，共 28 项。稳定 registry 包含 30 个必选 case；28 个操作的完整映射由独立 public-header inventory 测试校验。Disk 的分区擦写属于后续独立资格领域，不计入本 App。

Storage 必须分两次独立进程或 boot 执行：seed 阶段执行 27 项并提交确定的数据，verify 阶段执行 3 项，读回与 nonce 绑定的文件及全部 Preferences 类型并清理专用数据。宿主只在两阶段 case 清单、nonce、版本、返回值和 cleanup 全部匹配时授予资格。Desktop/macOS 使用真实 OS FS 和 SQLite，Web 使用 IDBFS/localStorage 并重新启动整个浏览器，移动端测试 App 使用 SDK 包内的原生 storage owner，在两次独立 App 进程间保留 sandbox。DevKit 使用板载 Flash LittleFS，BK7258 使用 SD FATFS 和 FlashDB；板级测试数据限定在 `/data/pal-storage` 和 `h2storea`/`h2storeb`，`h2storectl` 只保存本测试版本绑定的阶段元数据。

命令与平台证据见 `projects/e2e/apps/pal-storage/README.md`；iOS/Android 模拟器入口分别为 `make bazel-test-ios_pal_storage_simulator_test` 和 `make bazel-test-android_pal_storage_simulator_test`。外部设备与已启动模拟器的测试是 `manual`，其结果不能由旧缓存代替。正常重启验证不表示断电原子性或物理介质寿命已验证。

## PAL MQTT

`projects/e2e/apps/pal-mqtt` 持有 MQTT 八个公开 operation 的固定 36-case registry。Portable App 只借用 Runtime 中的真实 MQTT、Time、Memory 等能力；平台入口拥有 provider、网络端点和 lifecycle。验收覆盖 QoS0/QoS1、匹配 packet ID 的 ACK、多订阅/unsubscribe、二进制与大消息、同步提交后的输入 lifetime、认证拒绝、真实 TLS 的错误 CA/hostname、连接/keepalive timeout、远端断线、reconnect、capacity 恢复和重复 lifecycle。公开 API 没有 cancel，也不声明 QoS2、持久 session replay 或 TLS resumption 已通过。

`projects/e2e/libs/pal-mqtt-fixture` 提供真实 MQTT 3.1.1 wire 对端，维护订阅路由、QoS ACK、retained 状态和各 case 的可控失败行为。TLS 由本轮测试 CA/certificate 提供，错误 CA/hostname 必须同时有真实 ClientHello、Certificate 和失败 handshake 的 witness；可信证书不能取得拒绝证据。Fixture 禁用 session ticket/resumption，所有 MQTT client 必须回到各自 allocation baseline，最后 teardown 检查 client、retained message 和资源清理。受控协议 subset 的通过不表示任意生产 broker 互通已完成。

CoreMQTT 的私有传输在同一配置 deadline 内发送完整向量，继续处理真实短写和 `WOULD_BLOCK`，只计已接受的字节；超时保留 PAL timeout。Connect 使用连接预算，publish/subscribe/unsubscribe/disconnect 使用请求或 operation 预算，process 的短轮询预算保持独立。固定 10 ms 的 vendor 重试间隔不能截断仍在 PAL 操作预算内推进的分段发送。BK 在资源测量前 stop/join 管理会话，完整首份 ledger 通过原生 SDK console 输出后再恢复管理服务和确认 App；资源门槛仍严格比较 before/after。

iOS/Android 消费实际 SDK 包导出的显式 MQTT owner；owner 持有本平台 Net/WolfSSL 引用、四个 incoming/outgoing QoS1 record 和借用 allocator，失败创建须回收已取得的资源，busy destroy 保留 owner 供重试。测试 launcher 显式注入其 MQTT API，AppHost 默认 assembly 保持 canonical unsupported。移动端核对 SDK public header/factory 符号、iOS IPA/XCFramework 或 Android APK/AAR 的实际字节，并检查两次分配失败、全部 36 case、native resource balance 和 provider/core teardown。每轮真实 JSON 配置、CA、registry 与 artifact/hash manifest 保留本轮身份；临时 TLS 输入已清理后不能倒推补造旧证据。

硬件独立入口为 `//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/devkit:package` 与 `//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405:package`。DevKit launcher 用真实 ESP Net/MbedTLS 与 PSRAM allocator 创建八个 incoming/outgoing record 的 coreMQTT，并在 suite 结束后销毁 provider、核对其实际 allocation 释放；BK 入口显式启用现有 native coreMQTT 开关，仍使用 board Runtime 的真实 provider。两者都要求受管升级与正常重启两次独立 36-case、对应 broker witness、原 Loader 与 coredump 保留、P2 有效且 Stage 清空。LAN fixture 默认 2 秒 TLS 握手预算，硬件诊断可显式指定其它预算并记录实际耗时和错误；新的 fixture 输入与固件包保持独立身份。

Desktop 直接运行 `//projects/e2e/targets/cc_binary/pal-mqtt:desktop_test`；移动端分别使用 `//projects/e2e/targets/ios_application/pal-mqtt:ios_pal_mqtt_simulator_test` 和 `//projects/e2e/targets/android_binary/pal-mqtt:android_pal_mqtt_simulator_test`。公网 `:public_broker_test` 为 `manual`/`external`，同一 App 的单个 QoS0 round trip 只授予该子集通过，不能代替完整 registry。连接成功但没有 CONNACK 的负例预算由 launcher 注入，仍须 broker 实际观察到 CONNECT，并验证有限 deadline 的上下界；未到达 broker 的 TCP timeout 不满足该负例。入口、命令与平台能力边界见 `projects/e2e/apps/pal-mqtt/README.md`；缺少 raw TCP/TLS MQTT provider 的 Browser 保持明确 unsupported，不能把 host 结果当作六平台资格。

## PAL Crypto E2E

`projects/e2e/apps/pal-crypto` 独立覆盖 Crypto PAL 的 15 个操作和 22 个必选 case。macOS、WASM、iOS、Android、DevKit、BK7258 的入口分别位于 `targets/cc_binary/pal-crypto`、`targets/pkg_tar/pal-crypto`、`targets/ios_application/pal-crypto`、`targets/android_binary/pal-crypto` 和 `targets/h2loader_tar_zlib/pal-crypto/{devkit,bk7258_v3_202405}`。结果必须含完整 case ledger，全部 PASS 且 failed/blocked/not_run/rc 为零才 qualified。Host/browser/mobile runner 使用外部超时，设备有独立诊断 watchdog 并保留 H2Loader 串口服务；设备 replay 只重放当前 boot 的不可变结果，不算新执行。

```sh
bazel test //projects/e2e/apps/pal-crypto/app:interface_coverage_test \
  //projects/e2e/apps/pal-crypto/app:rejection_test \
  //projects/e2e/targets/cc_binary/pal-crypto:desktop_test \
  //projects/e2e/targets/pkg_tar/pal-crypto:browser_test
H2_IOS_SIMULATOR_UDID=<booted-uuid> make bazel-test-ios_pal_crypto_simulator_test
H2_ANDROID_SERIAL=emulator-5580 make bazel-test-android_pal_crypto_simulator_test
```

移动端从实际 Swift Package/AAR 导入 provider，Android 比较 APK/AAR 内 `.so` 字节；iOS/Android 结果来自模拟器。每次硬件安装前查询 UID/Loader/coredump 基线，使用 H2Loader managed serial send 校验 Stage 包/镜像 SHA 后正常升级，最终核对有效 App、空 Stage 和未改变的 Loader/coredump。测试不输出生成的私钥或随机字节，也不把功能验收当成密码认证。

## PAL Net/TLS

`projects/e2e/apps/pal-net-tls` independently qualifies the raw `h2_pal_net.h` core profile with 37 mandatory cases and two explicit optional capability cases. The 21-operation inventory includes bounded asynchronous DNS, UDP/TCP/source bind, listen/accept, full byte streams, TLS trust/name/expiry rejection, SNI/ALPN peer evidence, deadlines and session recovery. Certificate rejection requires typed `TLS_VERIFY` and this run's observed ClientHello, emitted Certificate and no application payload; a post-handshake expiry rejection also requires a zero-payload close. Generic IO and bad endpoints cannot pass. Host/mobile and board resolver views have independently recorded, timestamped operator DNS expectations; both sync and copied-host async PAL answers must exactly match their declared expected address. Committed board evidence is typed JSON with local raw-source SHA references and byte-identity snapshots; raw logs stay local. HTTP/Fetch/WebRTC do not replace raw Net/TLS evidence.

The mobile consumer imports actual native SDK packages and injects the public owned Net provider. Device launchers borrow saved Wi-Fi configuration, keep H2Loader service, confirm only complete success and require two independent boots with Loader/Stage/coredump preservation. Browser executes the actual Worker/AppHost unsupported raw Net diagnostic boundary, reporting core qualification false. The six-platform capability assessment is complete when supported capabilities pass and genuinely unsupported capabilities are explicitly skipped. Browser raw Net/TLS remains `SKIP` with `core_qualified=false`, never functional PASS; its actual 21-operation unsupported probe, Worker identity and teardown are verified. Browser-local semantics are preserved without a remote relay. Optional ICMP and multicast membership are exercised where implemented: DevKit validates a real echo, and macOS/iOS/Android/BK validate membership setup. Missing callbacks are explicit skips. Multicast delivery, IPv6, NETIF binding, DTLS, and OS public-root trust are separate scopes. Source/artifact-bound receipts and direct `bazel test` commands with exact labels, platform configs and explicit simulator `--test_env` inputs are described in `projects/e2e/apps/pal-net-tls/README.md`. Bazel calls the shared Python fixture/device lifecycle engine; that engine does not schedule Bazel. Wi-Fi qualifies before raw Net/TLS integration begins; fake/oracle/fixture self-checks are not platform E2E evidence.