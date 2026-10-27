# Mosaico PAL E2E 测试支持

本次交付让现有 PAL 及其消费者 E2E 在 Mosaico 上运行。范围以
`projects/e2e/apps` 的工作负载为准，另保留 Camera PAL 的最小消费者；
Lingxi、OneNET 暂不接入、不计入本次验收。它不承诺整板外设或产品功能验收。

## 分层与入口

| 层级 | 职责 | 测试入口 |
| --- | --- | --- |
| `libs/pal` | 稳定能力 contract、Camera borrowed-frame contract 和 unsupported 实现 | 共享 PAL contract 回归 |
| `libs/runtime` | 注入真实 provider；Camera 未配置时使用 unsupported；无隐式采集生命周期 | Runtime 注入/生命周期回归 |
| `native_component_src/esp-idf6.x` | ESP 网络、音频和 S31 libco 栈保护适配 | 各平台组件回归 |
| `boards/esp_mosaico/esp32s31` | GPIO/总线/器件 wiring、驱动组合、通用单 CDC 传输 | 板级驱动回归，不增加 E2E 包 |
| `projects/h2loader` | 产品 Loader、Mosaico 协议与 CDC 绑定 | 现有 `h2loader-serial` App |
| `projects/e2e/apps`、`libs` | 可复用用例、runner、受控 fixture；保留公共断言 | 全量清单，不按 `pal-` 前缀过滤 |
| `projects/e2e/targets` | Mosaico 原生 launcher、任务策略、布局及 fixture 注入 | 配置化 package、独立 GizClaw package |
| `tools/bazel/smoke/esp32s31` | 芯片工具链/链接检查，compile-only | 不烧录成 Mosaico 固件、不计 E2E 包 |

不存在 `peripherals`、`chip`、`loader` 三个独立 E2E suite。
传感器、电池、按键、触摸、PWM 驱动保留在 BSP，绑定既有 PAL 接口；
不建立聚合 peripherals PAL、不安排独立马达验证。
板层只拥有通用 CDC，Loader 注入位于
`projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_mosaico_usb`，
板层不反向依赖产品 Loader。

## 构建与清单

```sh
bazel run //projects/e2e/tools:list_apps -- --repository "$PWD"
bazel build --config=esp32s31 --define=mosaico_e2e=core \
  //projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico:package
bazel build --config=esp32s31 \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/esp_mosaico:package
```

通用入口允许 `core`、`storage`、`display`、`audio`、`libco`、`camera`、
`json`、`crypto`、`audio-decoder`、`net-tls`、`http`、`mqtt`、`webrtc`、
`wifi`、`ipv6`、`atomic`、`iperf`、`iperf-client`、`iperf-server`、
`lua-runtime`、`lua-link`、`webrtc-performance`、`aec-calibration`。
未知配置在分析期拒绝，不静默回退 Core。输出路径复用，按 suite/version/hash 保存实际产物。
SDK 与工具版本遵循仓库现有固定策略，不修改外部 SDK 或 BSP 源码。

## 保留的适配与用例修复

- Core `wall-set` 接受 provider 初始 `UNCALIBRATED`，经真实 PAL 设置测试基线后继续
  原时钟断言；不再需要 Mosaico 启动器注入人工 UTC。该用例会改变 RTC 校准状态。
- S31 libco 在切换栈时同步硬件 guard、RTOS 栈及协处理器元数据；平台操作在 native
  component，RV32 backend 仅提供平台 hook，其他平台默认空实现。
- Storage 保留跨三次独立启动的 seed / verify-remove / clean-verify。
  Camera 保留十次 start/stop、每次三十帧、借帧 BUSY 和 release/cleanup。
- Display 比较已完成 SPI DMA 的字节；Audio 保留原 24 项和录放稳定性。
  字节/PCM 证据不自动等同于光学质量或声学产品验收。
- Mosaico 单 CDC 串行化日志与管理输出；只有 Loader 读取 RX。DTR 下降清理旧会话，
  TinyUSB endpoint ready 决定传输可用性，保留半帧和断链错误。
- 网络入口显式注入服务地址、证书、Wi-Fi fixture，短事务使用既有 power-save NONE。
  Net/TLS 增加真实组播收发证据；不修改 DevKit/AMOLED 的组播实现。
- Mosaico UDP mailbox 为 32、TCP/IP mailbox 为 64、动态 RX 缓冲为 64；TX 聚合保持 SDK
  默认开启。撤下 TX 聚合关闭、PSRAM/static-TX 等失败实验，不放宽性能门限。
- WebRTC 回声保留 RTP 序号、时间戳和 marker；丢帧标记不计作收到的音频帧，
  仍按 missing、duplicate、reorder、P99 判定。消费回调先取时，不同步打印逐包缺口。
- Lua shutdown 先观察真实 WAITING，再在有限期限内 join；GizClaw URL 重装使用 HTTPS
  fixture，不将 Binary upload 的 data URL 当作安装地址。不削弱生命周期断言。

## 受控服务与对端

GizClaw 使用独立 Mosaico target，复用完整十项 workload、API 审计及 cleanup。
服务地址、注册凭据、信任根、AppConfig、Lua archive/golden 均通过 fixture 输入；
GizOS 不反向依赖 firmwares/H106。测试用确定性 AI provider 仅部署于本地测试服务，
不修改正式服务或设备断言；通过证明协议、音频流及生命周期，不代表云模型质量。

Lua Link 使用已有 AMOLED Join；iperf-server 使用独立 AMOLED Client 夹具，
三个地址族矩阵与各项传输保持原期限。AMOLED 的 AEC 声源仅作为独立测试夹具，
70% 音量用于原探针；DUT 两组候选、两档幅度、五阶段形成二十个观察窗口。
AEC 通过仅对测试摆位及探针条件有效，不自动写入产品校准参数。

WebRTC 性能使用现有性能协议服务器。可选
`--define=H2_MOSAICO_PERF_BSSID=<BSSID>` 只固定本次连接的 AP，
不改写保存凭据。正常交付不包含并行小 UDP 探针、私有 Wi-Fi TX 统计、
逐帧跟踪或 lwIP linker wrap。测试期间单独监听日志，避免升级确认轮询干扰计时。

## 结果与验收

2026-10-27 收敛依据为 25 个 suite 的已通过真机记录；Lingxi、OneNET 保留待测。
准确 case 数和各次镜像身份见 [issue #708](https://github.com/GizClaw/gizos/issues/708)。
既有结果绑定实际源码/配置/镜像，不能自动改称后续清理镜像已完成全量真机复测。

WebRTC r17 smoke 通过：audio-only 99/100、P99 183.641 ms；loaded 100/100、
P99 186.479 ms；吞吐比 137.7%，无 300 秒超时。相同配置曾有失败，
该记录不证明 HAIVIVI-MFG 无线链路已稳定或丢包根因已根治。
保留原 200 ms P99、每百帧最多五帧缺失、零重复/乱序、80% 吞吐比和 300 秒期限。

包结果按 case 汇总：全部成功即成功；有失败记失败；未执行保持待测试。
成功显示“成功，N/N case 成功”；case 原因只写“成功”。失败原因说明实际失败断言，
记录指向对应日志。安装、确认及返回 Loader 作为执行链路证据，不覆盖 case 结论。

执行前核对设备身份、独占串口、布局与备份；镜像准入记录 source/diff、SDK/config、
version/image SHA。原始 Flash、凭据及设备原始日志保留私有。结束后核验恢复状态；
返回已知 Loader 不代表全 Flash 已恢复，更不代表 RTC/产品状态完全还原。
