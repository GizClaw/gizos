# E2E Apps

这里保存可由不同平台 launcher 复用的 headless E2E App。App 负责公共 case、结果、有限执行预算和资源清理；launcher 负责真实 Runtime/provider、测试 fixture、平台生命周期与证据采集。当前 App 清单和 ownership 规则见 [E2E 指南](../../../guides/apps/e2e.md)，各 App 的 README 保存自己的接口和验收合同。

## PAL E2E v2 平台范围

PAL E2E v2 当前已有测试入口的矩阵为六个平台：

| 平台 | 当前验收入口 |
| --- | --- |
| Desktop | 主线为 macOS；Linux/Windows 的宿主扩展覆盖另行登记 |
| WASM | 真实浏览器中的 WASM / pthread Worker |
| ESP | ESP32-S3 DevKit 或 AMOLED，具体板由套件确定 |
| BK | BK7258 |
| iOS | iOS Simulator，消费实际打包的 SDK |
| Android | Android Emulator，消费实际打包的 SDK |

各套件依自己的能力合同记录功能 PASS 或 unsupported 边界。后续待补的平台覆盖统一为 Linux、Windows、JieLi 三类；JieLi 之前没有进行 v2 验收，现纳入后续平台接入范围。

## 旧混合 PAL E2E 下线

2026-10-09 下线旧 `pal` App 及其 Desktop、Browser、DevKit Preference、AC707N 和 AC791N launcher，包括旧 package 转发入口与 MQTT Make/shell 入口。移除范围以 `c4012704` 的源码为基线；不会保留调用旧实现的兼容 label。独立 PAL 套件继续拥有各自的 portable contract：

| 旧 suite / 能力 | 当前替代 App | 合同与入口 |
| --- | --- | --- |
| Core、Memory、Time、Timer、Task、Queue、Sync、System Event | [pal-core](pal-core/README.md) | Core v2：46 个操作、41 个必选 case；macOS host、Browser、移动端与 DevKit/BK7258 专用入口 |
| FileSystem、Preferences 和跨启动持久化 | [pal-storage](pal-storage/README.md) | 契约 2：28 个操作、36 个必选 case；seed、verify、clean-verify 与再次启动检查 |
| Resolver、UDP、TCP、TLS | [pal-net-tls](pal-net-tls/README.md) | 39 个 case、37 个 mandatory；Browser 单独验证 raw Net/TLS 的真实 unsupported 边界 |
| HTTP/HTTPS | [pal-http](pal-http/README.md) | 45 个 case；真实 HTTP/HTTPS fixture、请求/响应、TLS 拒绝和清理 |
| MQTT loopback / public broker | [pal-mqtt](pal-mqtt/README.md) | 完整 36-case TCP/TLS fixture；独立 `public_broker_test` 运行 public smoke |
| STA 断开后状态、Netif、网络状态事件 | [pal-wifi](pal-wifi/README.md) | 独立 Wi-Fi/Netif 合同；区分真实 radio 能力与宿主 unsupported 检查 |

`Live E2E` 的 `pal` / `all` scope 直接运行新版 public broker test，仍只做 public smoke，不能据此宣称完整 MQTT 资格：

```sh
bazel test --config=macos_arm64 --cache_test_results=no \
  //projects/e2e/targets/cc_binary/pal-mqtt:public_broker_test
```

该入口继续使用 `H2_MQTT_SMOKE_*` 配置。`scripts/test/test-web.sh` 显式运行独立 Core、Storage、HTTP 和 Wi-Fi/Netif 测试。raw Net/TLS E2E 与其 Browser boundary target 保持 `manual` / `external`，按需直接指定 label 执行，不进入 CI 或 Web 批量测试入口。没有旧 `pal.web.tar` 或旧 Browser launcher。

## 待补的平台覆盖

缺口仅登记 Linux、Windows、JieLi 三类平台。独立 App 已存在、包已构建或 provider-local 单元测试通过，都不能自动关闭对应平台的 Runtime E2E 缺口。

| 缺口 | 移除的覆盖与当前边界 | 补齐要求 |
| --- | --- | --- |
| Linux Core / Netif / System Event | 旧 Host 在 Linux 跑 Time、Task、Queue、Mutex、Semaphore、Condition、Netif 和 System Event；新 Core host launcher 仅声明 macOS 兼容，Wi-Fi/Netif host 入口也仅支持 macOS。Linux provider-local 测试继续存在。 | 为独立 Core 和 Netif 套件接入 Linux 的真实 provider，完成该平台 case ledger、资源清理与默认自动测试。 |
| Windows Runtime PAL 集成 | 旧 Host 同时覆盖 Windows Core、FS、DNS、UDP/TCP、TLS、HTTPS、MQTT、Netif 和 System Event；新的 Core、Storage、HTTP、MQTT、Net/TLS host launcher 尚未声明 Windows 兼容。Windows provider 单元测试、compile/link smoke 继续存在。 | 为独立套件添加真实 Windows launcher/fixture，运行相应合同并按既有自动/manual 分类调度；raw Net/TLS 保持手动执行。 |
| JieLi | 尚无独立 PAL 套件的 JieLi launcher 和 v2 实板验收记录；旧入口清理涉及 AC707N/BR35 和 AC791N/WL82。 | 纳入后续平台接入范围，按具体板的能力接入对应独立套件，完成实际运行 ledger 与资源清理验证。 |

IPv6 接入及相关验证由独立任务负责，本次不新增专门的 IPv6 UDP 用例，也不将其列为平台缺口。raw Net/TLS 本来就按手动测试运行，不在 CI 执行；`manual` / `external` 分类是既定调度边界，不属于待修复项。

## JieLi 旧入口清理与后续接入

JieLi 当时没有进行 PAL E2E v2 验收。本次删除的是旧混合 PAL 体系的以下入口：

- AC707N / BR35：旧 Core launcher、普通 firmware 和 H2Loader package；BR35 SDK/toolchain、board layout 和 PAL provider 保留。
- AC791N / WL82：旧 FS/Core/Wi-Fi launcher 与 SDK FS probe，包括该诊断 App 的未确认/复位返回 Loader 路径。

这些删除不构成已有 JieLi v2 验收的回退；JieLi 后续接入作为上表的平台缺口登记。板级 bring-up、provider-local 测试与 H2Loader 验证继续按其各自范围维护。

DevKit 的旧 Preference 流程由独立 Storage 的跨启动合同接替。旧指南曾列出的 Tiga V4.2 `pal-pref` target 在下线基线中已不存在，不能视为当前可运行覆盖；如需该板资格，应为独立 Storage 增加对应 launcher 并执行真实跨 boot 验证。

历史 hardware receipt、qualification JSON、guide evidence 和经过哈希认证的目录 baseline 保留原样。它们可能引用已退役的路径或 label，只证明记录中绑定的源码和 artifact，不是当前运行入口，也不能重新绑定为本次删除后的验证结果。
