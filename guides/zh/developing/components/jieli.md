# JieLi Components

杰理 (JieLi) 芯片按产品线发布独立 SDK（AC695N 蓝牙音频、AC707N BR35、AC79 WiFi+BT 等），但共用一套私有 LLVM/Clang 工具链与 Linux post-build 打包工具。GizOS 用**一条工具链 repository、一个 `jieli_firmware` external build rule、按系列区分的 SDK locator 与 platform** 接入它们：`target = br23` 对应 AC695N，`target = br35` 对应 AC707N，`target = wl82` 对应 AC791N。本文定义该 rule 的 contract、artifact-root ownership、执行边界与烧录/升级边界，以及 BR23/BR35 PAL core provider。

## 工具链形态

- 杰理工具链是私有 clang 4.0.1（`-target pi32v2 -mcpu=r3` 或 `-target q32s`），仅发布 Windows 与 Linux x86_64 预编译二进制；一个 tarball 同时包含 `common/`、`pi32/`、`pi32v2/`、`q32s/`。AC695N/AC696N/AC701N/AC707N/AC791N 都使用 `pi32v2/bin`，AC63/AW31N 使用 `q32s/bin`。
- post-build 工具（`isd_download`、`fw_add`、`ufw_maker`、`packres`、`remove_tailing_zeros` 等）是 Linux x86_64 Qt 程序，必须以 `QT_QPA_PLATFORM=offscreen` 运行。
- 上游下载短链每次重定向到最新版本且不保留历史，因此 tarball 由 firmwares-devenv 镜像（`tools/jieli_toolchain/archives`）并由 `make jieli-toolchain` 解包到 `.tools/jieli-linux-toolchains-20250805.1`、`.tools/jieli-linux-post-build-tools-20260728.1`（`--strip-components=1`，`packres` 改名 `pack_res`）；`tools/bazel/native_versions/jieli_toolchain_archives.txt` 固定 archive SHA-256 与 bootstrap 产出的 expanded-tree SHA-256，`@h2_jieli_toolchain` 与 `@h2_jieli_postbuild` repository 只消费 `JIELI_TOOLCHAIN_ROOT`、`JIELI_POSTBUILD_ROOT` 指向的解包树并用 `toolchain_identity.py` 校验，不从网络下载。CI 用 deploy key sparse-checkout firmware-devenv 的 `tools/jieli_toolchain` 并运行同一个 `bootstrap.sh`。
- 链接需要 `ulimit -n >= 8192`；runner 在调用 Make 前提升该限制并 fail closed。

## pi32v2 compatible graph

`--config=ac695n` / `--config=ac707n` / `--config=ac791n` 构建完整 compatible graph：仓库里每个没有平台门控的 `cc_library` 都会用 pi32v2 toolchain 编译。clang 4.0.1 对 `{0}` 聚合初始化的误报由 toolchain 的 `unfiltered_compile_flags` 在每个 target 的 copts 之后统一关闭；真正编不了的目标用 `PI32V2_UNSUPPORTED_ARTIFACT_COMPATIBILITY`（`tools/bazel/platforms/compatibility.bzl`）显式退出 pi32v2 graph，并在 BUILD 注释里写明原因，而不是静默跳过。工具链升级后应收回这些标记。

## SDK locator

| 系列 | 变量 | 指向 | 验证 |
| --- | --- | --- | --- |
| AC695N (`br23`) | `JIELI_AC695N_SDK_PATH` | `h2vivi/AC695N_Soundbox_SDK` checkout 内的 SDK 根目录 `…/jieli_ac695n_sdk/SDK`（firmware-devenv 导出） | 所属 Git checkout 的 `native_versions/jieli_ac695n_sdk_commit.txt` exact commit、tracked cleanliness、`cpu/br23/sdk_ld.c` 与 post-build inputs 存在 |
| AC707N (`br35`) | `JIELI_AC707N_SDK_PATH` | `h2vivi/e_badge_707_sdk_200` checkout 内的 SDK 根目录 `…/jieli_ac707n_sdk/SDK`（firmware-devenv 导出） | `native_versions/jieli_ac707n_sdk_commit.txt` exact commit、tracked cleanliness、`cpu/br35/sdk_ld.c` 与 post-build inputs 存在 |
| AC791N (`wl82`) | `JIELI_AC791N_SDK_PATH` | `h2vivi/fw-AC791N_SDK` checkout 根目录（即 SDK 根） | `native_versions/jieli_ac791n_sdk_commit.txt`、cleanliness、`cpu/wl82/sdk_ld.c` 与 post-build inputs 存在 |

三个 SDK 均由 h2vivi 私有 mirror 固定；AC695N 与 AC791N mirror 来自杰理代理商云芯 (`gitcode.com/yunthinker`) 的发布仓，AC707N mirror 来自杰理 GitLab 的 e-badge 2.0.0 仓。`fw-AC791N_SDK` 用 Git LFS 托管二进制，执行构建的 host（CI runner 或 dev container）必须安装 `git-lfs`，否则 runner 的 cleanliness 检查会把 LFS 文件判为已修改并 fail closed。`.bazelrc` 的 `--config=ac695n` / `--config=ac707n` / `--config=ac791n` 只用 `--repo_env` 把对应变量交给 repository rule；变量未设置时 repository 注册为 disabled locator，firmware target 因 compatibility 被跳过，不阻塞无关 graph。

## Bazel external rule

`jieli_firmware(name, target, board, image, project_makefile, graph, srcs)`：

- `target` 只能是 `br23`、`br35` 或 `wl82`；macro 按 target 绑定 SDK locator、commit 文件、本地 post 脚本（`tools/bazel/jieli/local_post_<target>.sh`）与 SDK 子目录，调用方不得覆盖。
- `project_makefile` 必须是 `boards/<board>/<chip>/layouts/<profile>/project.mk` 中的仓库文件。它拥有完整 compiler flags、defines、include paths、SDK source inventory、linker inputs、generated files 与 output paths；SDK application/demo Makefile 不得成为 input，也不得被 include。
- Runner 把 SDK 子树复制到 invocation-local 目录（排除 `.git`、`doc`、`ui_project`），在 SDK 根执行 layout-owned project，并用 `TOOL_DIR=<pi32v2/bin>` 覆盖 `/opt/jieli` 默认值。SDK 只提供 source/header/archive/linker/post-build substrate。随后仓库自有 post 脚本用 objcopy、`isd_download`、`fw_add` 与 `ufw_maker` 生成发布输出。
- 固定输出 `firmware/firmware.elf`、`symbols.txt`（objsizedump 符号表）、`jl_isd.bin`（完整 NOR flash 镜像）、`jl_isd.fw`、`update.ufw`（USB 虚拟盘 / SD 卡 / OTA 升级包）与 `manifest.json`；`JieliFirmwareInfo`、`DefaultInfo.files` 与 `OutputGroupInfo.release` 暴露相同文件。Native rule 不返回 `FirmwareReleaseInfo`；managed entry 由外层 `h2loader_tar_zlib` 消费此 provider 并拥有 package 与 release metadata。
- Action 在当前 runner 上 unsandboxed、non-remote-exec 执行，不设置 `local`，声明 4 CPU / 4 GiB；只读取 allowlist environment（fixed PATH、`QT_QPA_PLATFORM=offscreen`、invocation-local `HOME`/`TMPDIR`），不继承 caller `PATH`。成功结果进入 local/GCS action cache。Action 不执行 flash、串口或设备操作。
- **执行平台只能是 Linux x86_64。** Rule 的 compatibility 同时要求 `h2_firmware_target` 与 `h2_host_os=linux`，macOS host 上 `bazel build --config=ac695n //...` 把 firmware target 标为 incompatible 并跳过；macOS 开发者在 Linux dev container 内运行整个 Bazel。Runner 自身也在触碰 SDK 前拒绝非 Linux x86_64 host。不在 Bazel action 内包装 `docker run`。
- `graph` 与 `srcs` 沿用其他 external rule 的语义：`firmware_native_component` 的 transitive source 进入 action key；`firmware_lib_component` archive 会被收集为输入，但在 SDK Make 显式消费 `H2_BAZEL_ARCHIVES` 之前不会注入最终链接。

## PAL core provider（br23）

`native_component_src/jieli/br23/h2_pal_core` 为 AC695N 实现 Memory、Log、Time、Sync、Queue、Task、Timer 与 Firmware Info 的 PAL provider：

- Provider 只依赖 `h2_jieli_br23_sdk_port.h` 这一层最小 SDK 接口（SDK heap、`put_buf` 调试串口、`timer_get_ms`、`os_time_dly`、`os_mutex_*`/`os_sem_*`、`os_task_create`、`sys_timer_add`/`sys_timeout_add`），`src/h2_jieli_br23_sdk_port.c` 是唯一 include SDK 头文件的翻译单元，由 `jieli_firmware` 的 native 构建编译；host 测试链接 `tests/` 下的确定性 fake，`bazel test //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 在任意 host 运行。
- Time 用 32 位 `timer_get_ms` 扩展为 64 位单调时间，wall time 不支持；Sync 提供 mutex（非递归）与 counting semaphore，condition variable 返回 `H2_PAL_ERR_UNSUPPORTED`；Queue 是 SDK mutex + 两个 semaphore 守护的堆环形缓冲，支持超时、`send_latest` 合并与 `close` 唤醒；Task `start` 走 `os_task_create`（任务返回后 park），`join` 不支持；Timer 用 `sys_timer_add`（周期）与 `sys_timeout_add`（一次性）——SDK 把回调派发到**注册该定时器的任务**上，`sys_timer_del` 不撤回已入队的回调，因此 timer 归属**第一次 start() 的任务**（其它任务再 `start()`/`reset()` 返回 `H2_PAL_ERR_INVALID_STATE`，归属不可转移）：`destroy()` 必须在该任务上调用（按 `xTaskGetCurrentTaskHandle()` 句柄判定，任务名可能重名；其它任务调用返回 `H2_PAL_ERR_INVALID_STATE`），SDK timeout 槽用尽时返回 `H2_PAL_ERR_UNAVAILABLE` 并保留 timer 供重试而不释放，它只置 `destroyed` 标志并在同一任务上注册一次性 reclaim timeout 释放存储，已入队的回调看到标志后直接返回；Firmware Info 报告 wrapper 注入的 `H2_JIELI_FIRMWARE_VERSION`。
- os_api 的 pend 超时以 tick 计且 0 表示永久等待，PAL 的 0 表示不等待，因此 port 用 `os_*_accept` 实现非阻塞尝试，并按 10 ms tick 换算毫秒。

## Repository-owned native project

AC695N、AC707N 与 AC791N 的 `compile_only` layout 直接拥有 `project.mk`、`app_config.h`、TASK policy、系列所需的 interrupt 配置与最小 board composition；具体 firmware launcher 自己提供 `app_main`。Runner 在 invocation-local SDK 根执行 `make -f <layout>/project.mk h2_link`，但 project 只选择 SDK 的 CPU/common substrate、headers、archives、linker inputs 与 post-build inputs，不编译 `apps/soundbox/**`、`apps/demo/**` 或其它 SDK application project。`tools/bazel/jieli/h2_project_rules.mk` 只提供 Bazel native object/archive 的通用追加规则，不 include SDK Makefile。每个 `firmware_native_component` 源文件使用 layout project 的 flags 与 Bazel include roots 编译到 `$(BUILD_DIR)/h2_bazel/`，`firmware_lib_component` archive 以 link group 进入同一条 `lto-wrapper` 链接。Bazel archive 是非 LTO ELF object，SDK source object 是 LTO bitcode。

## 板与 artifact entry

- `boards/ac695n_chip/ac695n/layouts/compile_only/`、`boards/ac707n_chip/ac707n/layouts/compile_only/` 与 `boards/ac791n_chip/ac791n/layouts/compile_only/` 拥有裸芯片验证 project。
- `jieli_firmware` 是底层 external build rule；`targets/jieli_firmware` 是现有
  reference-smoke artifact root。后者只保留 AC695N/AC791N 的完整 native link
  smoke，不作为新 portable App 的通用入口，也不为 SDK 自带 demo 建 entry。
- 新增 standalone 厂商固件使用
  `projects/<owner>/targets/native_firmware/<image>/<board>/:firmware`。它拥有
  image/board 选择并直接暴露 `JieliFirmwareInfo` 中的 ELF、symbol、NOR、FW、UFW
  和 manifest；它不表示 H2Loader 已能安装或启动该镜像。
- 同一 App 的 H2Loader package 使用
  `projects/<owner>/targets/h2loader_tar_zlib/<image>/<board>/:package`，消费同一
  `JieliFirmwareInfo` 并把 UFW 放入 format-1 package。standalone 与 package entry
  必须复用 `projects/<owner>/native_component_src/jieli/<family>/<image>/` 的 launcher
  graph，不能复制 App main、PAL provider 或 SDK glue。
- 两种 artifact 都继承底层 `jieli_firmware` rule 的 Linux x86_64 execution 与 target
  compatibility；macOS 通过 Linux/amd64 container 构建，不引入 macOS 原生 vendor
  toolchain contract。

## 烧录与升级边界

- 空片首次烧录不在 GizOS 范围：需要杰理强制升级工具（Windows，芯片 USB 枚举为 UBOOT 设备）或一拖二烧写器（AC791N 带 KEY 烧录仅支持一拖二）。
- 日常迭代用 `update.ufw`：设备虚拟 U 盘拷贝、SD 卡升级或 HTTP/FTP OTA。
- `jl_isd.bin` 是完整 flash 镜像，供烧写器或有 KEY 的首刷流程使用。

## AC791N DevKit composition

`boards/jieli_ac791n_devkit/ac791n/` 描述物理开发板，区别于 `ac791n_chip` 的 compile-only 验证配置。`layouts/h2loader/` 集中拥有 SDK config、NOR geometry、启动配置与 SDK patches；`h2loader_jieli_firmware` 注入这些输入，各 firmware entry 选择自己的 launcher graph 和 task policy，不能单独覆盖 project makefile 或 SDK patches。

`native_component_src/jieli/wl82/h2_pal_core` 提供 SDK port 与 PAL core 实现，host test 使用 fake SDK。Board 组合 Display、Touch、ADC Button、Wi-Fi、BLE、Audio、SD filesystem 和 Preference；硬件 pin 与 SDK 配置由 board header 和 layout 文件拥有。UART1 的 TX 为 PB3、RX 为 PA6，Loader command 与日志复用该链路，使用 460800 波特率。BLE 使用 SDK host/controller，默认不主动发起配对或保存 bond。

FDK AAC 编译由 `libs/fdk_aac` 拥有，PAL decoder 依赖该 first-party library；`@h2_fdk_aac` 仅暴露 upstream source group 和 header-only target，不引用 GizOS platform labels。pi32v2 的无 stdio 编译选项在 first-party library 内选择，Linux 保留原始 stdio 行为。

wl82 condition 为每个 wait 创建独立的 SDK semaphore，signal/broadcast 只通知当时已经注册且尚未收到通知的等待者；超时退出会注销自己的节点，不把 token 留给后来的等待者。等待者队列用短时间持有的原子 gate 保护，竞争时让出任务；节点在 SDK wait 返回之前始终保持注册，因此 destroy 会拒绝仍有等待者的 condition。与 PAL contract 一致，wait 只接受非递归 mutex，返回前重新取得调用者 mutex。

wl82 的 pi32v2 clang 没有可内联的字长原子读改写指令，C11/GCC 原子操作都会降级为 `__sync_*` libcall。工具链 compiler-rt 的实现用裸 `lockset/lockclr` 包住读改写；SDK 自己的 SMP spinlock 已改用 `testset`（`asm/cpu.h` 把旧的 `lockset` 写法放在 `#if 0` 下，它需要每核嵌套计数）。在双核上 compiler-rt 版本会丢更新：PAL system event 的生命周期字从 ACTIVE（`0x80000000`）变成 `0x7fffffff`，此后所有订阅失败，BLE Loader command service 约三分之一的启动无法打开。`h2_jieli_wl82_sdk_port.c` 为 1/2/4/8 字节的 `__sync_*` libcall 提供强定义（asm label 绑定 libcall 符号，`used` 保留到 LTO 之后），每个操作由 SDK `spin_lock` 保护；链接时它们优先于 compiler-rt 归档成员，因此 PAL、board、portable library 与 SDK 代码共用同一实现。单核 AC695N 不受影响。

wl82 SDK 的 `errno` 是 `apps/common/system/init.c` 中的单个全局 `int`，`__errno()` 返回它的地址；FreeRTOS 未启用 `configUSE_NEWLIB_REENTRANT`，newlib `_impure_ptr` 同样全局共享。两个核上的所有任务共用同一个 `errno`，socket 或 libc 调用失败后读取 `errno` 可能读到其它任务写入的值。当前 Loader 与 App 镜像不使用网络；在 AC791N 上启用 Wi-Fi、HTTP、GizClaw 或 h2peer 前，必须先把 `errno` 改为按任务存储，或让 board net provider 不依赖全局 `errno`。

wl82 Queue 的 ring、数据数量和关闭状态由同一把非递归 mutex 保护；readable/writable condition 只通知线程重新检查条件，不在锁外预占数据或空位。`reset` 在锁内丢弃待处理数据并唤醒等待空位的发送者，不重新打开已关闭的队列；`send_latest` 的追加或替换在一次持锁期间完成。`close` 唤醒所有收发等待者，拒绝后续发送，但允许接收者排空已有数据。调用者必须先 close 并结束所有使用者，再 destroy。有限等待跨多次唤醒共用一个超时预算。Queue 通过 wl82 内部 `h2_jieli_wl82_cond_wait_owned` 获取错误返回时的锁归属：SDK 重新加锁失败会返回 IO，Queue 不再尝试解锁；这不改变公共 PAL API。真实 pthread Queue 测试覆盖 reset 与接收访问交错、reset 唤醒发送者、多等待者关闭及关闭后排空，fake 回归覆盖重新加锁失败；它们不能替代板级性能与完整 Loader 生命周期验收。

TinyH264 的 pi32v2 allocator bridge 使用 SDK port 的 task identity 与 sleep 接口，按任务查找当前 allocator；每个作用域的节点由调用栈持有，enter/leave 对称登记和注销，不分配全局固定容量槽、不占用 SDK TLS 槽，也不依赖 `pthread_once`。登记表只在修改和查找时短暂加锁，解码及 allocator callback 在锁外执行；同一任务的嵌套作用域退出后恢复上一层，不串用其它 decoder task 的 allocator。其它平台保留原有 thread-local 路径。

H2Loader host 仍下载 `tar.zlib`，不是直接下载 UFW。Package 内的 `app/jieli/update.ufw` 是 native updater 消费的 image；`h2loader_tar_zlib` 从 `JieliFirmwareInfo` 取得它。原生 `jl_isd.bin` 用于独立的 USB DL 恢复流程，不等同于 managed package。Native rule 本身不取得 release identity；外层 package rule 拥有 package metadata。JieLi package 不生成 ESP/BK 格式的 recovery bundle。

物理 NOR 为 8 MiB：`[0, 0x700000)` 由 SDK double-bank packer 管理，Loader/App 是逻辑角色，不是两个固定地址的裸 flash 分区；`[0x700000, 0x740000)` 为 Preference，`[0x740000, 0x780000)` 为 coredump，`[0x780000, 0x7ff000)` 为 vendor reserved，最后 4 KiB 为 boot reserved。`h2_jieli_ac791n_devkit_partitions.h` 是容量与边界的 source of truth；下载文件位于 SD filesystem，不能把 SD 容量当成可执行 NOR 容量。

`App → Loader → 已安装 App` 使用 board-layout-owned 热启动：ROM 保留选择稳定 Loader 的原生 BootInfo，App 安装不发布自己的 BootInfo，返回 Loader 时直接复位而不擦除正在运行的启动信息。Loader 校验 P2 元数据和镜像影子后，可在没有新 Stage 的情况下写入试运行证据并请求 P2；软件复位后，layout 的早期 hook 消费有校验的单次 RAM handoff，切换固定 SDK 布局的 SFC 窗口并重建原生入口交接。启动消费者只链接进 Loader，App 不重复消费请求；公共 Loader 状态机仍拥有安装、确认和命令行为。RAM handoff 只跨软件复位，持久 boot intent 和 trial attempt 由 Preference 保存，二者不可混为掉电保证。完整生命周期测试之外，仍须单独验证无新 Stage、无重新写入的已安装 App 启动。

Loader 在 SD 卡 `/dl` 下保存 image 原始字节影子，用于 Partition 2 校验与 self-update 回写；影子不能放在 `/data`，因为安装 App 时 image writer 完成后会清空 App data root。Trial 回滚以 Preference 中的 `jieli_trial_attempt` 为证据：请求 P2 App 或候选 Loader 前记录 image checksum，成功确认后清除；回到 P1 时若 attempt 仍匹配 P2，则报告该候选不可启动，包括无 Stage 的 App 试运行。仅有 Stage 等于 P2 不是失败证据。update semaphore 在每次启动时创建一次、不再删除，超时后迟到的 burn callback 只会 post 仍然有效的 semaphore，writer begin 会先清零。Retained 崩溃记录携带来源镜像：只有 Loader 自身的断言或看门狗才进入降级恢复（阻止 App 自动启动、跳过 BLE），trial App 崩溃经 rollback 回到 Loader 时 BLE 照常启动。

SDK 的 `dual_bank_updata_api.h` 公开新镜像校验及 BootInfo 写入、清除和读取接口；[官方 AC79 升级说明](https://doc.zh-jieli.com/AC79/zh-cn/master/module_example/system/update.html)不作为任意选择已有 bank 的保证。热启动是固定 WL82 SDK/board layout 的适配，不是新增公共 SDK API。Loader 自更新仍复用 SDK updater，通过本 layout 的完整 NOR adapter 暂存候选 P2 启动头，将其与镜像身份一起持久化；候选经热启动确认后，才在公共回写 P1 之前发布 P2 启动头。正常更新及确认前复位恢复的硬件证据、具体 SDK ABI 和剩余断电验收见 [AC791N DevKit H2Loader](/apps/h2loader/boards/jieli_ac791n_devkit/h2loader)。不能用正常更新或主机 mock 测试替代提交中断、实际断电和失败恢复的实机验证。

声明的 `sdk_patches` 只应用于 invocation-local SDK 副本，原始 SDK checkout 不被修改。Firmware、ELF、symbols、manifest 由 Bazel action 发布；手工硬件诊断的日志不属于发布产物。

## AC791N validation commands

PAL BLE 诊断包位于 `//projects/e2e/targets/h2loader_tar_zlib/pal-ble-smoke/jieli_ac791n_devkit:package`，通过相同的 `h2loader_jieli_firmware` wrapper 使用正式 board layout，保留 UART App command 通道与分步日志，并带 `no-release` tag。不再维护独立的 vendor demo config、Makefile 或 BLE 实验 patch；该诊断只用于定位 PAL 调用阶段，不替代 H2Loader BLE 生命周期验收。

Host 验证：`bazel test //native_component_src/jieli/wl82/h2_pal_core:test_jieli_wl82_platform_core //projects/h2loader/libs/h2loader:all //projects/h2loader/apps/cli/app:all //projects/example/apps/mp4-player/app:mp4_player_test`。

Linux x86_64 构建与 e2e-runner 验收命令见 [AC791N DevKit H2Loader](/apps/h2loader/boards/jieli_ac791n_devkit/h2loader)；BLE 控制器配置（DLE、2M PHY、MTU 512）、PHY 断言缺陷及复验结论也记录在该页。真机验收必须分别检查 UART/BLE 基础命令、App 安装与确认、return-to-loader、没有新 stage 时再次启动同一已安装 App、Loader self-update、失败恢复，不能用基础命令通过代替完整 lifecycle 验收。再次启动已有 App 时必须确认没有重新上传或重写镜像，同时保留 Loader 的恢复能力。

## Reference validation

- `bazel test //tools/bazel:jieli_runner_test //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 在任意 host 运行。
- Linux x86_64：`. ../firmwares-devenv/export.sh && bazel build --config=ac695n //projects/e2e/targets/jieli_firmware/reference-smoke/ac695n_reference:firmware`，并以 `--config=ac791n` 构建对应 AC791N target；重复构建应命中 action cache。
- macOS：同一命令应报告 target incompatible 而非失败。
- 真机：空片首刷与 `update.ufw` 升级在开发板到位后各验证一次，记录于对应 board 文档。

## AC707N (BR35) 工具链

`--config=ac707n` 选择 `//tools/bazel/platforms:ac707n` 和独立的
`gizos_jieli_ac707n_cc_toolchain`。复用已固定的 pi32v2 clang 4.0.1，
按 BR35 SDK 添加 `-mllvm -pi32v2-large-program=true`、`-fdiscrete-bitfield-abi`；
其他杰理平台的编译参数保持原样。BR35 原生链接使用 `pi32v2/lib/r3-large`。

SDK locator 使用 `JIELI_AC707N_SDK_PATH`，由 firmware-devenv 导出为
`third_party/jieli_ac707n_sdk/SDK`，固定到
`h2vivi/e_badge_707_sdk_200@d0167685d032d745d88fe50233302edd46941622`。
这是 2.0.0 系列吧唧 SDK；头文件位于 `interface`。

`boards/ac707n_chip/ac707n/layouts/compile_only` 拥有最小原生工程，使用 SDK 的
BR35 startup、CPU/system/config/VM/FS/device 库和链接脚本。仓库 launcher
启动 UCOS 后组装 Runtime，调用现有 `projects/e2e/apps/pal/app:pal_e2e` 的 core suite。
测试结果保留在 `h2_ac707n_pal_e2e_result`；未启用的外设使用 PAL unsupported provider。

在 Linux x86_64 开发环境（macOS 使用 Linux/amd64 容器）内运行：

```sh
bazel build --config=ac707n //projects/e2e/targets/h2loader_tar_zlib/pal/ac707n_chip:firmware
```

firmware-devenv 的 `make jieli-ac707n-toolchain-check` 另行验证 BR35 参数下
的编译及 `r3-large` 运行库链接，不代表硬件运行验收。

输出目录为 `bazel-bin/projects/e2e/targets/h2loader_tar_zlib/pal/ac707n_chip/firmware/`，
包含 `firmware.elf`、`symbols.txt`、`jl_isd.bin`、`jl_isd.fw`、`update.ufw` 和固定依赖版本的 `manifest.json`。
CI native build matrix 包含 AC707N。编译、链接与离线打包可验证；没有开发板，
启动、时钟、Flash 配置和升级包的硬件适配尚未验收。当前 compile-only 配置
采用 8 MiB Flash、24 MHz 晶振和 PB07 reset，实际板卡必须另建并验证板级配置。

普通固件入口是 `//projects/e2e/targets/native_firmware/pal/ac707n_chip:firmware`，
遵循上文定义的 standalone vendor artifact contract，而不是新增 SDK demo 或新的
底层 firmware rule。
H2Loader 入口 `//projects/e2e/targets/h2loader_tar_zlib/pal/ac707n_chip:package`
产出 H2Loader format-1 tar.zlib，固件成员为
`app/jieli/update.ufw`。这只验证封装和依赖图；设备端 H2Loader UFW 安装、
Loader/App 选择及回退还需要 BR35 专用 backend，不能把普通 SDK 双 bank OTA
当作已完成的 H2Loader 启动协议。

BR35 PAL core 在 `native_component_src/jieli/br35/h2_pal_core`，使用 UCOS，
仅 CPU0；互斥量采用二值信号量，零等待在关中断区域查询并消费已有 token，
不依赖未导出的 `os_sem_accept` / `os_mutex_accept`。任务 join 由创建者回收，
条件变量使用逐等待者信号量。SDK sys_timer 回调仍属于注册任务；PAL task queue
专用，sleep 期间通过 `os_taskq_pend_timeout` 派发回调。真机时序尚未验证。
