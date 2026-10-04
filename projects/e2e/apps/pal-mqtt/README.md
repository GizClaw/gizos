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

当前独立 artifact entry 支持 macOS/Linux；实际资格须按当次测试和 artifact/source hash 判断。Browser 目前没有 raw TCP/TLS MQTT provider，不能算 mandatory MQTT PASS。iOS/Android SDK 消费、ESP32-S3 和 BK7258 的独立 entry 与真实运行证据尚未取得；没有将 host 结果改称六平台通过。IPv6 由独立延期任务负责。

旧 PAL App、loopback 和 public smoke 入口保留，直到对应平台、执行 scope 和 CI 迁移有完整证据。新 suite 通过不意味着旧 PAL 全部可以退役，也不把历史 MQTT receipt 改绑到新 App。

## Mobile SDK consumer

移动端入口分别是 `//projects/e2e/targets/ios_application/pal-mqtt:ios_pal_mqtt_simulator_test` 和 `//projects/e2e/targets/android_binary/pal-mqtt:android_pal_mqtt_simulator_test`，复用 `tools/bazel/mobile_e2e.py` 的明确设备身份、安装、fixture、报告与 cleanup。测试声明 `manual`/`external`，要求 `--nocache_test_results --local_test_jobs=1`；iOS 通过 `--test_env=H2_IOS_SIMULATOR_UDID=<allocated UDID>`，Android 通过 `--test_env=H2_ANDROID_SERIAL=<allocated emulator>` 和显式 `ANDROID_HOME`/`ANDROID_NDK_HOME`。Runner 不 boot、reset 或 erase 模拟器。

这次为 SDK 新增真实 `h2_ios_mqtt`/`h2_android_mqtt` owner，原 SDK/AppHost 的 MQTT 是 unsupported。Owner 持有独立 Net/WolfSSL 生命周期引用、4/4 QoS1 records 和可选 borrowed allocator；E2E 消费实际 XCFramework/AAR 的 factory，核对 public header、SDK/IPA symbols 或 APK/AAR binary identity，再执行 36 个相同用例。每个进程另外真实验证 owner 和 coreMQTT 分配失败回滚、owner destroy、native resource before/after、最终零 tracked allocation 与 Core shutdown。Host unit lifetime test 用 Net reference stub 检查 busy destroy 保留/重试，仅作为 SDK owner unit test，不能替代真实网络或移动证据。

移动 hook 保留当轮 fixture JSON、CA、wrong CA 与 registry 的 SHA256 输入 manifest，以及实际 SDK/App artifact hash和真实 peer witness。新增移动资格必须以新 consumer artifact 与新 SDK 包的实际运行记录为准，不能改绑历史 SDK receipt。
