# PAL MQTT E2E

独立 portable App 通过 Runtime 的真实 MQTT provider 运行固定 36-case registry，覆盖 MQTT 的八个公开 operation。App 持有 case、deadline、事件/ACK 断言、非 fail-fast 汇总和 client cleanup；host entry 持有真实 Darwin/Linux Net、coreMQTT、WolfSSL、allocator 追踪和 broker 注入。没有引用 legacy 混合 PAL App，也没有 fake PAL transport。

## 自动覆盖

- 旧 MQTT round trip 的 open、CONNECTED、SUBACK、QoS0 publish/echo、DISCONNECTED 和 close 全部保留；公网 smoke 复用同一个 `publish-qos0` case，并明确只授予该子集通过。
- QoS0/QoS1、匹配 packet ID 的 PUBACK/SUBACK/UNSUBACK、多 filter、同步提交返回后覆写非 NUL terminated filter/topic span 与 payload、空消息、257-byte 二进制消息和 4097-byte payload。
- unsubscribe 后确实没有 echo、retained 订阅投递和 retained 清除、空闲 process deadline、无连接操作、重复 connect/disconnect、非法参数和不支持的 QoS2。
- 真实 TCP、真实 TLS、受控用户名/密码接受与拒绝、错误 CA 和错误 server name 的证书拒绝、没有 CONNACK 的连接超时、broker 主动关闭、keepalive 没有 PINGRESP、同一 handle reconnect、20 轮 open/connect/subscribe/publish/disconnect/close。
- Launcher 按 provider 实际配置注入 QoS1 capacity；未处理 ACK 时填满 outgoing records，检查失败事件、处理 ACK 后恢复。publish callback 内 close 验证延迟回收和 callback quiescence。

MQTT 当前公开 enum 只提供 QoS0/QoS1，公开 API 没有 cancel operation。App 不构造不存在的 cancel 合同，也不声明 QoS2、跨线程使用、配置 span 提前释放或持久 session replay 已验证。coreMQTT 的 client config、TLS config 和 network buffer 一直借用到 close；收到的消息/ACK span 只在 callback 内读取。

## 真实受控对端

`projects/e2e/libs/pal-mqtt-fixture` 是自包含的 MQTT 3.1.1 wire broker，监听进程独占的 loopback ephemeral ports。它解析真实 CONNECT、SUBSCRIBE、PUBLISH、PUBACK、UNSUBSCRIBE、PINGREQ 和 DISCONNECT，按订阅表路由消息，记录 QoS ACK 和 retained 状态，通过独立 client ID case suffix 选择拒绝认证、静默 CONNACK、断线与丢弃 PINGRESP。它提供协议 subset 的受控对端，不等同于完整生产 broker 的互通资格。

TLS 使用当次运行生成的两天有效 test-only CA/certificate。验收同时核对严格有序完整 case ledger、零失败/blocked、各 case allocation baseline、最终零 allocation/invalid free、broker arrival 和零 live client/retained message。错误 CA 和错误 hostname 各需服务端观察到 ClientHello、实际 Certificate 和失败 handshake。额外 negative test 把错误 CA 替换成可信 CA，要求 App 的证书拒绝 case 和 fixture witness 都拒绝该伪证据。

受控 TLS fixture 禁用 session ticket/resumption，避免把 WolfSSL 正常的 process-wide ticket cache 计入本轮 client allocation baseline；本 suite 不声称 TLS resumption 已验收。每个 MQTT client 必须回到同一 allocation baseline，最后 provider destroy/deinit 必须回到零。

## 直接入口

```sh
bazel test --config=macos_arm64 //projects/e2e/apps/pal-mqtt/app:interface_coverage_test //projects/e2e/apps/pal-mqtt/app:reject_missing_test //projects/e2e/targets/cc_binary/pal-mqtt:desktop_test //projects/e2e/targets/cc_binary/pal-mqtt:public_entry_test //projects/e2e/targets/cc_binary/pal-mqtt:tls_rejection_evidence_test
```

Linux 使用 `--config=linux_x86_64`。本地 fixture 测试没有外部服务依赖和 tag；Bazel 持有构建、测试和缓存策略，没有额外 Make alias 或 Python Bazel 调度器。Python launcher 只管理真实 peer、进程和 evidence。

公网替代入口声明 `manual`/`external`，不复用测试结果，构建缓存保持启用：

```sh
bazel test --config=macos_arm64 --nocache_test_results //projects/e2e/targets/cc_binary/pal-mqtt:public_broker_test
```

该入口保留 `H2_MQTT_SMOKE_HOST`、`H2_MQTT_SMOKE_PORT`、`H2_MQTT_SMOKE_TLS`、`H2_MQTT_SMOKE_TOPIC_PREFIX` 和 `H2_MQTT_SMOKE_TIMEOUT_MS` 的原有含义，默认仍为 `broker.emqx.io:1883`、TCP 和五秒 budget。使用 `--test_env=<name>` 明确传入所选配置；`H2_MQTT_SMOKE_CA_FILE` 可选，用于注入测试 CA。每轮使用新的 crypto nonce 避免复用 topic 或接收别人历史消息。自动 `public_entry_test` 在 loopback TCP/TLS 分别执行这个实际 launcher，不访问公网。

## 平台和退役边界

当前独立 artifact entry 支持 macOS/Linux、iOS Simulator、Android Emulator、BK7258 与 DevKit ESP32-S3。macOS、指定 iOS Simulator、Android Emulator 和定向 DevKit 实板已有完整 36/36 运行与清理证据；DevKit 取得两次不同 nonce 的完整启动资格并恢复原 App、非空 Stage 和设置。Linux 尚未实跑；BK 已实际执行，最新为 34/36 FAIL，尚未 qualified。Browser 目前没有 raw TCP/TLS MQTT provider，不能算 mandatory MQTT PASS；没有将 host 或 simulator 结果改称六平台/实体手机通过。IPv6 由独立延期任务负责。

旧 PAL App、loopback 和 public smoke 入口保留，直到对应平台、执行 scope 和 CI 迁移有完整证据。新 suite 通过不意味着旧 PAL 全部可以退役，也不把历史 MQTT receipt 改绑到新 App。

## Mobile SDK consumer

移动端入口分别是 `//projects/e2e/targets/ios_application/pal-mqtt:ios_pal_mqtt_simulator_test` 和 `//projects/e2e/targets/android_binary/pal-mqtt:android_pal_mqtt_simulator_test`，复用 `tools/bazel/mobile_e2e.py` 的明确设备身份、安装、fixture、报告与 cleanup。测试声明 `manual`/`external`，要求 `--nocache_test_results --local_test_jobs=1`；iOS 通过 `--test_env=H2_IOS_SIMULATOR_UDID=<allocated UDID>`，Android 通过 `--test_env=H2_ANDROID_SERIAL=<allocated emulator>` 和显式 `ANDROID_HOME`/`ANDROID_NDK_HOME`。Runner 不 boot、reset 或 erase 模拟器。

这次为 SDK 新增真实 `h2_ios_mqtt`/`h2_android_mqtt` owner，原 SDK/AppHost 的 MQTT 是 unsupported。Owner 持有独立 Net/WolfSSL 生命周期引用、4/4 QoS1 records 和可选 borrowed allocator；E2E 消费实际 XCFramework/AAR 的 factory，核对 public header、SDK/IPA symbols 或 APK/AAR binary identity，再执行 36 个相同用例。每个进程另外真实验证 owner 和 coreMQTT 分配失败回滚、owner destroy、native resource before/after、最终零 tracked allocation 与 Core shutdown。Host unit lifetime test 用 Net reference stub 检查 busy destroy 保留/重试，仅作为 SDK owner unit test，不能替代真实网络或移动证据。

移动 hook 保留当轮 fixture JSON、CA、wrong CA 与 registry 的 SHA256 输入 manifest，以及实际 SDK/App artifact hash和真实 peer witness。新增移动资格必须以新 consumer artifact 与新 SDK 包的实际运行记录为准，不能改绑历史 SDK receipt。

`connect-timeout` 由 launcher 注入 stage budget：host 默认为 200 ms，移动端、BK 与 DevKit 为 2000 ms，使真实 TCP setup 有机会完成。App 检查 elapsed 不低于 configured budget，且不超过两个 stage budget 加 500 ms 调度余量；broker 必须实际收到同一 execution 的 CONNECT 并保持 CONNACK 静默，TCP setup 本身超时不能代替该负例通过。

## BK7258 LAN fixture and package

BK 入口为 `//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405:package`，使用真实 board Runtime 的 coreMQTT provider，其 incoming/outgoing capacity 是 8/8、allocator 是 `h2_bk_platform_default_allocator()`。Standalone runner 使用 64 KiB PSRAM task。资源测量前停止并 join 独立管理会话，避免正在读取 Pref 的控制缓冲进入 MQTT 基线；原生 SDK console 继续输出 fresh BOOT 和 ledger。成功或失败后都恢复 UART/Wi-Fi command service，stop/restart 错误保持明确 FAIL。Launcher 从编译配置注入精确 IPv4 host/ports、session prefix、CA/wrong CA 与 epoch，每次执行另取真实 Crypto nonce，实际计算 CA SHA256并校准 wall time，36-case 完成和 native resource before/after 严格平衡后才确认 App。

LAN fixture 默认保留 2 秒服务端 TLS 握手预算，可通过 `serve --tls-handshake-timeout` 显式设置硬件诊断预算。Inputs receipt 记录实际预算，实时 receipt 对未合格执行也保留每次握手的 peer、ClientHello/Certificate、TLS 消息、实际耗时和错误；不会把超时或缺少证书交换的连接当成证书拒绝证据。每次改变 fixture 输入都应建立独立 CA/session/epoch/端口与新包，不重写旧执行记录。

2026-10-04 的 BK 实跑记录见 [failed.json](../../targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/evidence/failed.json)。`mqtt-bk-decc30e1-r5` 保留全部 36 case，可信 TLS 握手与消息往返、错误 CA/hostname 均通过；普通 TCP 的 `connect-events` 与 `remote-disconnect` 在连接阶段返回 IO，最终 34 PASS、2 FAIL。Native counters 从 25/51916 降到 24/51460，属于未满足资源基线平衡，不能直接解释为泄漏。该轮只把 `pal-mqtt` ledger 交给 native SDK console，portable runner 仍通过 PAL Log，生产 Log/Net/MQTT 默认实现未变。此前 r2/r3 的可信 TLS IO、r4 的 36 PASS 重放但初始 BOOT 缺失均保留独立身份；一次 broker 成功或已有重放不能代替完整验收，失败根因尚未确认。

最终已受管恢复原 `gizclaw-dev-r58-socketerrno` P2、原 P1 与原空 Stage，32-byte coredump 保持原字节。为避免原 GizClaw 测试随重启自动播放，当前主动停在 Loader；这与原 App 运行角色分别记录，见 [restored-final.json](../../targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405/evidence/restored-final.json)。Loader 的最终 Wi-Fi 查询实际失败 `code=-7 terminal=0`，不声称该 getter 已验证；本任务未写 Wi-Fi 设置。临时 native TLS diagnostics 已移除，历史诊断包与失败记录保持原 source/artifact 身份。

`//projects/e2e/libs/pal-mqtt-fixture:serve` 必须显式给出 `--bind`、`--advertised`、`--bazelrc` 和 `--receipt`，只监听指定 IPv4 interface 上的独占 ephemeral TCP/TLS ports。服务生成 test-only CA 与 build defines，不包含 Wi-Fi secret；真实设备使用已有 saved STA 配置。服务按本轮 nonce 区分 wire arrival、ACK、两次证书拒绝与零 live client/retained message，完整 witness 第一次成立时冻结该 run，后续 boot 不能改写旧 receipt。此服务不安装、重启、扫描或读取串口。

只读 direct verifier `//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/bk7258_v3_202405:device_test` 消费主任务统一完成的定向安装与监视数据。要求 `H2_MQTT_DEVICE_PORT`、`H2_MQTT_DEVICE_UID`、`H2_MQTT_DEVICE_EVIDENCE_DIR` 和本轮实际安装的 `H2_MQTT_DEVICE_PACKAGE`；目录必须有 `managed.log`、`normal.log`、`before-status.log`、`after-status.log`、两份 `*-coredump-status.log` 和 `fixture-receipt.json`，非空 dump 另需真实 `coredump-before.bin`/`coredump-after.bin`。验收两次不同 nonce 下同 boot 的完整 36-case，任何之后出现的 BOOT/STARTUP、不同 version 或 nonce 都使旧 ledger 失效；并严格核对 fresh UID、原 P1 valid/role/package/image hash、P2 valid=1与 app/package/image/version、清空 Stage和实际 coredump 状态/bytes不变。默认 unit tests只验证该 oracle 的拒绝行为，不能当作物理 BK PASS。

## DevKit ESP32-S3

独立入口为 `//projects/e2e/targets/h2loader_tar_zlib/pal-mqtt/devkit:package` 与 host 只读 `:device_test`。Launcher 直接创建完整 lifecycle 的 `h2_coremqtt`，使用既有真实 ESP Net/MbedTLS、Time、Log、PSRAM allocator 与 8/8 records；DevKit board 默认 MQTT 仍是 unsupported。64 KiB PSRAM worker 执行同一份 36 case，检查 suite 前后实际 native counters 相等，销毁 provider 并检查其 allocation 释放后才确认 App。CA digest 由 ESP-IDF PSA SHA256 callback 提供，portable App 不依赖目标 SDK。

早期实跑 managed package 的固定 source 是 `955dcfcef0f5050b4a5a97d7e5e53be71cf0e22c`，version `mqtt-esp-abi-r3`，package SHA256 `563fdb6e9adde23023ededcca5685538cc71a396aca20cccc97894af0ee4fd47`，image SHA256 `f9c30bd870c45ff1299cae93d722044fd98043a4e1cb2a124cd04703c71e9798`。独立默认 2-second LAN fixture 实际 source 是 `ad04417f1b80455ca50963b9acf2f77b486e4ae6`；host oracle 是 `b569cf92a1fabbdc028d3edff20b663bcb3e7ae6`。三种输入 scope 分开保留，host-only 改动不改绑实际 App source。

两个 qualified boot 都完成 36/36、三次真实 TLS handshake/两次证书拒绝、完整 CONNECT/ACK/message witness、零 broker client/retained、native before==after、provider_cleanup=0 与 confirm=0；严格 host verifier 核对原 P1、实际 P2 image/package/version/valid、Stage 清空及 coredump。资格 snapshot 保持在恢复前 Stage=0；恢复后原 R35 App 和原 Stage=1、设置、WiFi 与 coredump 同安装前一致。

实际 CA/config/source 原始输入与 UART/host receipts 保存到本轮证据目录。初始两次 compile 失败、两次被 host parser 拒绝的 UART capture、缺工具链和新构建 image-mismatch 的 Bazel 失败保持独立且未 qualified。最终只读 `device_test` 必须显式消费本轮已安装的 immutable package，不为验收重建另一镜像。结构化结果见 `targets/h2loader_tar_zlib/pal-mqtt/devkit/evidence`。

## Recorded qualification

| Platform | Actual execution source | Result | Receipt |
| --- | --- | --- | --- |
| macOS host | current focused test invocation and App/binary inputs | 36/36, strict wire witness and zero final resources | Bazel `desktop_test` output |
| iOS Simulator | fixed `da7704d9495c9da568cb0ed878395e5b7496ef01`, including configured send deadlines | 36/36; actual IPA/XCFramework, symbols/header, two allocation failures and owner cleanup | `targets/ios_application/pal-mqtt/evidence/runs/da7704d9` |
| Android Emulator | fixed `da7704d9495c9da568cb0ed878395e5b7496ef01`, including configured send deadlines | 36/36; actual APK/AAR, symbols/header, two allocation failures and owner cleanup | `targets/android_binary/pal-mqtt/evidence/runs/da7704d9` |
| DevKit ESP32-S3 | fixed artifact/fixture source `da7704d9`; host oracle `10b5cd48` | 36/36 on two fresh boots; exact peer/native/provider cleanup; upgraded P1 preserved, original App/Stage/WiFi/dump and empty Data restored | `targets/h2loader_tar_zlib/pal-mqtt/devkit/evidence/runs/da7704d9` |
| BK7258 | standalone P2 package and explicitly bound LAN fixture | latest R7 mandatory cases 36/36; second-boot resource baseline failed, full qualification pending repair | root-owned directed UART/install/status/dump and `device_test` |

Each mobile evidence directory preserves the original successful `qualified.json`/`environment.json` plus `executed-source-inputs.json`; fixture JSON/CA/wrong CA/registry and actual artifact/SDK hashes remain the observed values. The first iOS observation without a fixture-input manifest and Android's initial TCP timeout with no CONNECT witness remain separate ignored validation journals and are not relabeled as these qualified runs. Later BK-only adapters or documentation commits do not rewrite the actual mobile execution identity.

Device `status` 的权威输入是唯一 `H2_LOADER_STATUS` 行；当前真实 CLI 不在该行输出 `result`/`code`，verifier 不添加或要求这些不存在的 device 字段。每份 `before-status.log`、`after-status.log`、`before-coredump-status.log`、`after-coredump-status.log` 都配同名 `*-receipt.json`，独立 host receipt 必须包含实际 `command`、精确整数 `exit=0`、指定 `port`、aware UTC `started_at_utc` 和匹配原 log bytes 的 `log_sha256`，不能用 stdout 文本伪造进程成功。

`managed.log`/`normal.log` 配各自 receipt，command 分别是 `reboot upgrade --monitor` 与 `reboot app --monitor`。只有 `controlled_stop=true`、`stop_reason="validated complete ledger"`、真实 `exit_after_capture` 为 0/130/-SIGINT，并有不早于开始时间的 aware UTC `captured_at_utc` 才允许受控结束；若还记录 `exit`，两者必须一致。完整捕获前退出、因失败中断或其它错误码均不 qualified。原 log 必须有唯一且匹配 target 的 accepted reboot ACK，verifier 只接纳该 ACK 之后的 fresh BOOT/36-case/READY，并保持晚到新 boot 使旧 ledger 失效、fresh UID/P1/P2/Stage/实际 coredump bytes 的全部强 gate。

The `da7704d9` mobile receipts are new fresh executions after the common coreMQTT deadline change. Their exact original fixture JSON/CA/wrong CA bytes and actual SDK/App files are preserved in the independent evidence capsules. The prior `5fede880` iOS and `afc4b236` Android files remain historical receipts under their original identities. Android's first new-output-base Maven resolver timeout occurred before App installation/execution and is retained as an infrastructure failure; the successful run used this task's existing enabled repository/action caches and `--nocache_test_results`.

After the common send-deadline fix, DevKit ran the fixed `da7704d9495c9da568cb0ed878395e5b7496ef01` App/fixture (`mqtt-esp-da7704d9-r2`) on two new boots with the default 2-second TLS server budget. The immutable package SHA256 is `40b83628cedd612b058994a1b3264db0e77e5240f13ec7cf1a7c6360f0c62845`; image SHA256 is `8dfc91b86ec0f013e327efc9e250dde83e8d53fe13a7ac1baa40202d5a3dc364`. Both ledgers passed 36/36, exact wire/TLS proof, native resource balance, provider cleanup and confirmation, followed by an actually executed host `device_test`. The new dual-format P1 Loader image `7109a515...4ae136` and package `4a491867...79775f` are the fresh baseline and were preserved. Original R35 App/P2, Stage1, saved WiFi/state (except RSSI), blank coredump and independently read empty Data checksum were restored. The restore host command's actual 120-second timeout remains a failed receipt; final device state was verified by separate successful commands. The prior `955dcfce` hardware receipt remains historical and is not relabeled.
