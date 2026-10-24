# Mosaico 芯片与外设 E2E 执行计划

2026-10-22 用户授权补齐测试入口，并恢复使用 `/dev/tty.usbmodem101`（已进入 ROM 下载模式）。
此授权替代 #708 原有硬件暂停约束；不把下载端口当作已验证的板型身份。
PR 继续 Draft，未满足的物理验收保持 pending。固件备份和原始设备日志不提交公共仓库。

## 入口与复用

保留 S31 独立芯片入口和 Mosaico Loader。新增一个配置化 App 项目：
`//projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico:package`。
通过 `--define=mosaico_e2e=<suite>` 选择 `core`（默认）、`libco`、`storage`、
`display`、`audio`、`camera`、`peripherals`。所有配置复用一个板级构建与布局；不复制
现有 App 工作负载，不批量建立七套板级项目，不新增 `esp_idf_firmware` Mosaico 入口。
不同 suite 的包输出路径相同，构建后立即复制到私有证据目录并按 suite/hash 区分。

| 配置 | 复用/必要新增 | 验收边界 |
| --- | --- | --- |
| core | 原 DevKit Core launcher 的原生资源、时钟、日志、任务栈与故障注入观测；条件接入 Mosaico CDC | 原完整 registry，无 BLOCKED/NOT_RUN，cleanup=0，qualified=1；任务探针通过 |
| libco | `h2_libco_smoke_run` | 原有 wait/wake/timeout/cancel/join 与 10000 次切换，真实 RV32 后端；不改后端 |
| storage | `pal-storage-device:runner` | 同一 image version 连续三次独立启动：seed → verify/remove → clean-verify；不可在一次 Runtime 内替代重启 |
| display | `pal_display_e2e_run`、共享视觉演示函数与 SPI DMA capture hook | 按真实 RGB565/不裁剪能力运行原 registry、亮度演示及稳定图案；completed-SPI-DMA 比较仅证明传输字节，亮度和实际屏幕另需人工观察 |
| audio | `h2_pal_audio_e2e_run` | 原 registry、30 s 持续录放、清理通过；麦克风帧/峰值与声音人工确认分别记录，不宣称 AEC |
| camera | 新增最小 Runtime Camera consumer | 10 次 start/stop，每次 30 帧；1280×720 UYVY、stride/size、帧 hash、借帧 stop=BUSY、release/stop 重试；不自动销毁未归还资源 |
| peripherals | 新增直接 PAL 观测入口 | 100 次 BMI270/双 BMM150 和按键采样、只读电池、250 ms 30% 振动后归零、30 s 触摸窗口；采样不代表物理校准 |

新增 `:camera_consumer_test` 直接复用消费者源代码，覆盖完整 300 帧流程、取帧超时、坏帧、
归还失败保留借帧以及 release/stop 重试；显式保留优化构建的断言。

Camera 在专用工作任务内串行执行；1000 ms 仅为驱动 dequeue 超时。记录完整调用耗时，
不按 1000 ms 判定函数硬实时期限。缺少摄像头时保留首个错误，单独记录 absence 测试；
安装/拆卸仅在断电后进行。实际图像预览、场景变化与光学质量仍需单独观察，hash 不能替代。
所有新入口报告 `physical_qualified=0`；软件成功不能自动授予整板验收。

外设入口在每颗磁力计第一次 PAL 读取失败后，使用已有 BSP handle 做一次只读诊断，
记录补偿结果、8 字节数据寄存器及补偿参数；它不改变传感器模式、校准、PAL 返回值
或本轮失败结论。该入口独占 IMU 消费，诊断值仅用于区分传输失败与补偿溢出。
没有按键/触摸人工操作确认时，缺少事件保持交互验收待验证，不凭空认定硬件损坏。

libco 开始前在临界区读取并记录当前任务的硬件栈保护边界；诊断不改变保护配置或
RV32 后端。固定 SDK 的 coredump 配置明确提示 PSRAM 任务栈的 Flash dump 限制，
因此不预设启用 Flash coredump 就能取得现场。保留 ELF/map 与真实失败镜像身份；
没有有效异常现场时仍记录原因未知，不能仅凭首个阶段缺失认定是栈保护故障。

## 构建与软件验证

使用仓库固定 IDF commit 和工具版本文件，不修改 SDK。示例：

```sh
bazel build --config=esp32s31 --define=mosaico_e2e=core \
  //projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico:package
bazel build --config=esp32s31 \
  //projects/e2e/targets/esp_idf_firmware/reference-smoke/esp32s31_chip:firmware \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package
```

逐项构建七种 suite，核对组件图只引入所选工作负载，板级默认 Camera 保留。
运行板级 Camera/USB/资源生命周期/存储/显示/音频/peripherals host tests、Camera PAL、
Runtime/并发、复用 App 和 device runner 回归；运行格式与 diff 检查。DevKit Core launcher
与 Loader 回归构建，验证 Mosaico 条件接入没有改变原路径。记录本机工具环境失败，
不得将历史构建记录表述为本轮结果。

冷启动且未校时的设备会让 Core 的 `wall-set` 前置读取返回 `UNCALIBRATED`。
测试侧可显式注入 `--define=H2_MOSAICO_CORE_WALL_UTC_MS=<UTC Unix 毫秒>`，
仅在墙钟无效时通过真实 PAL 设置测试基线，并输出 `H2_PAL_CORE_TIME_FIXTURE`。
默认不注入、不伪造有效时钟，原 workload 断言保持不变；保留未校时首轮失败。
注入值及构建/启动时间应记录在产物证据中，这不是 NTP 精度验收。
时间用例会改变 RTC；原始 Flash 读回一致不代表 RTC 校准状态恢复。

## 设备前置条件与证据

1. 独占端口并检查是否有其他进程占用；不启动历史 controller。ROM 只读读取 chip/MAC、
   Flash 容量和安全状态，确认 ESP32-S31 及板修订。ROM MAC 不替代 Loader UID。
2. 写入前读取全 Flash 两次到私有目录，检查长度、SHA256 和逐字节一致性；记录安全位，
   确认备份可以读取。加密/安全启动或读取失败时停止写入，不降低恢复要求。
3. 固定 source SHA、dirty diff hash、SDK commit、编译器版本、suite、sdkconfig、
   包/镜像 SHA256、Flash 布局、设备 MAC/UID、板修订、端口、测试开始结束时间。
4. 确认 ROM 下载恢复路径可用；保留 bootloader、partition、NVS/Pref/数据分区的原始备份。
   先测试 Loader，再安装 App；不把独立 chip-smoke 的 4 MB compile-only 布局刷为整板布局。
5. 测试结束用原始布局恢复全 Flash，重新读取并逐字节核对，再观察原固件状态。
   仅在恢复与独立 readback 完成后标记 restoration PASS。

## 固定真机矩阵（执行前定值）

| 顺序 | 场景与负载 | 通过条件 / 期限 |
| --- | --- | --- |
| 1 | ROM 身份/安全位/双读备份 | 身份匹配、容量足够、两次备份完全一致；否则禁止写入 |
| 2 | Loader 枚举与只读管理，10 次连接，每次 status/help/stats | 每次 15 s 内握手，board/target/UID 一致；无意外 reset |
| 3 | 同一 CDC 日志+协议，60 s，日志 10 行/s，每行 128 bytes；1 MiB 固定种子 payload，3 次 | payload hash 一致，无帧交错/竞争 RX；每次传输 120 s 内；未有可用流量入口则 BLOCKED |
| 4 | 主机暂停读取 5 s，3 次；传输 25%/50%/75% 处断开，各 3 次 | 保留首个错误；半帧关闭会话，禁止旧队列污染；关闭/重开后 15 s 内重握手；区分自动/人工恢复 |
| 5 | 物理拔插 5 次、设备重启 5 次 | 每次 15 s 内重新枚举/握手，同 UID；需人工执行的动作保持待执行 |
| 6 | core / libco | 每个 3 次独立启动；Core 120 s、libco 180 s 的主机观察期限；无崩溃/看门狗/缺失结果 |
| 7 | storage | 3 阶段各一次独立启动，第二轮以新的 `--//tools/bazel:firmware_version=...` 镜像重新开始；phase 4 的只读重放不计新循环；每次 180 s 内，原有 registry 全通过 |
| 8 | display / audio | 各 3 次启动，每次 180 s；保留字节/PCM 软件结果，另记录屏幕/亮度、扬声器/麦克风人工结果 |
| 9 | peripherals | 每次 100 轮采样中按放 AI/BOOT 各一次（轮间至少 100 ms，另含 I/O 和日志耗时）；振动后进入 30 s 触摸 down/move/up；三轮，缺交互 BLOCKED；电池读数与外部仪表比较独立记录 |
| 10 | camera | 10×30 帧，3 次独立启动；180 s 主机观察期限，超时保留失败现场；断电拆卸后单独验证无相机 Loader 启动不受影响 |
| 11 | Loader/App 链路 | 选择通过的软件 App：安装→启动→确认→返回 Loader，3 次；每步 120 s；核对 active checksum、分区、stage_valid、last_result |
| 12 | 恢复 | 原 Flash 写回、全量 readback/hash 比较、原固件状态观察；未完成不关闭硬件验收 |

每项记录 PASS / FAIL / BLOCKED / NOT_RUN，不能将缺少人工动作算作 PASS。
设备 watchdog 只输出失败通知；主机观察期限是外部截止时间，不假装能取消上游阻塞调用。
先失败即保留该轮日志与镜像，后续重试不覆盖首个失败。问题按 launcher、GizOS provider、
BSP/SDK 或物理夹具归属记录，不能靠削弱公共 workload 的断言使测试通过。

## GizClaw 完整业务 E2E

Mosaico 另提供 `//projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/esp_mosaico:package`。
它复用现有 AMOLED launcher 的生命周期、配置、状态机、业务 workload 和 ledger，不复制业务实现；
Mosaico 负责单 CDC 初始化、板卡身份、S31 firmware SDK 选择及独立入口任务。
这层不能代替前述七个芯片/外设 suite，也不能用 H106 产品用例或其他板卡的历史记录代替。

默认选择完整十项：`lua-apps`、`resource`、`device-api`、`workspace-fence`、
`connectivity`、`rpc`、`firmware`、`voice`、`concurrency`、`service`，mask 为 `0x3ff`。
公共的旧 `H2_GIZCLAW_E2E_SUITE_ALL` 仅代表前八位；Mosaico 显式增加 Fence 和 Lua Apps，
不修改其他平台的默认集合。只有完整集合、所有 mandatory case、API 审计、ledger freeze 和零残留清理全部通过，
才允许 App confirm；诊断子集不授予完整资格。

构建使用 `--config=esp32s31`。服务配置继续通过已有
`//projects/e2e/apps/gizclaw:app_config_fixture_*` 显式输入；空配置可以编译，运行时在注册或资源 mutation 前失败。
GizOS 的构建、测试和运行不依赖 `firmwares` 仓库，也不读取 H106 产品 header、资源或 token。
H106 仅供对照环境分层和 profile 校验约定；GizOS 使用自己受控、由测试侧显式注入的 E2E 配置。
不把生产 endpoint 作为默认值，不复制 token 到源码、日志或公开证据。
本地验证可在上游私有目录保存一次性复制的测试配置，再显式注入；该副本不得成为 GizOS 的源码、默认配置或对 `firmwares` 的构建依赖。须记录实际 profile、endpoint、非敏感 AppConfig key/value、
HTTPS Device API/audio 对象身份，以及 Lua data/HTTP/HTTPS 包和 archive/entry/frame golden SHA。
缺少输入标记 BLOCKED，不能跳过断言或将缺少 fixture 的启动算作业务 PASS。

每份实际执行保留 source/artifact、nonce、逐 case 终态、API 审计及 cleanup/retained ledger。
完整 managed boot 和独立 normal boot 分别验收；真实麦克风/扬声器观察、Lua 持久化和显示 golden 继续按其原合同验证。
`libs/gizclaw` 与 portable App 的 host 测试属于软件验证，不能替代上述服务和真机执行。
