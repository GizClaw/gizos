# JieLi Components

杰理 (JieLi) 芯片按产品线发布独立 SDK（AC695N 蓝牙音频、AC707N BR35、AC79 WiFi+BT 等），但共用一套私有 LLVM/Clang 工具链与 Linux post-build 打包工具。GizOS 用**一条工具链 repository、一个 `jieli_firmware` external build rule、按系列区分的 SDK locator 与 platform** 接入它们：`target = br23` 对应 AC695N，`target = br35` 对应 AC707N，`target = wl82` 对应 AC791N。本文定义该 rule 的 contract、artifact-root ownership、执行边界与烧录/升级边界，以及 BR23/BR35/WL82 PAL core provider。

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

`jieli_firmware(name, target, board, image, project_makefile, graph, srcs, sdk_patches = [])`：

- `target` 只能是 `br23`、`br35` 或 `wl82`；macro 按 target 绑定 SDK locator、commit 文件、本地 post 脚本（`tools/bazel/jieli/local_post_<target>.sh`）与 SDK 子目录，调用方不得覆盖。
- `project_makefile` 必须是 `boards/<board>/<chip>/layouts/<profile>/project.mk` 中的仓库文件。它拥有完整 compiler flags、defines、include paths、SDK source inventory、linker inputs、generated files 与 output paths；SDK application/demo Makefile 不得成为 input，也不得被 include。
- Runner 把 SDK 子树复制到 invocation-local 目录（排除 `.git`、`doc`、`ui_project`），在 SDK 根执行 layout-owned project，并用 `TOOL_DIR=<pi32v2/bin>` 覆盖 `/opt/jieli` 默认值。SDK 只提供 source/header/archive/linker/post-build substrate。随后仓库自有 post 脚本用 objcopy、`isd_download`、`fw_add` 与 `ufw_maker` 生成发布输出。
- `sdk_patches` 是仓库内 `.patch` 文件的 label list，进入 action inputs，按声明顺序通过 `git apply --whitespace=error` 应用于 invocation-local SDK 副本，原始 SDK checkout 不被修改。路径越出 source root、文件不存在、后缀不符或 patch 无法应用都会使 action 失败。
- WL82 post-build 先清除副本中的旧打包输出，`jl_isd.bin` 发布 `-extend-bin` 生成的 `jl_isd_extend.bin` 完整镜像；`BR22_TWS_DB=YES;` 双 bank 模式下，`update.ufw` 承载 `db_update_files_data.bin`，单 bank 模式保留原有 UFW 打包流程。
- 固定输出 `firmware/firmware.elf`、`symbols.txt`（objsizedump 符号表）、`jl_isd.bin`（完整 NOR flash 镜像）、`jl_isd.fw`、`update.ufw`（USB 虚拟盘 / SD 卡 / OTA 升级包）与 `manifest.json`；`JieliFirmwareInfo`、`DefaultInfo.files` 与 `OutputGroupInfo.release` 暴露相同文件。不返回 `FirmwareReleaseInfo`，不进入 H2Loader package 或 GitHub Release matrix。
- Action 在当前 runner 上 unsandboxed、non-remote-exec 执行，不设置 `local`，声明 4 CPU / 4 GiB；只读取 allowlist environment（fixed PATH、`QT_QPA_PLATFORM=offscreen`、invocation-local `HOME`/`TMPDIR`），不继承 caller `PATH`。成功结果进入 local/GCS action cache。Action 不执行 flash、串口或设备操作。
- **执行平台只能是 Linux x86_64。** Rule 的 compatibility 同时要求 `h2_firmware_target` 与 `h2_host_os=linux`，macOS host 上 `bazel build --config=ac695n //...` 把 firmware target 标为 incompatible 并跳过；macOS 开发者在 Linux dev container 内运行整个 Bazel。Runner 自身也在触碰 SDK 前拒绝非 Linux x86_64 host。不在 Bazel action 内包装 `docker run`。
- `graph` 与 `srcs` 沿用其他 external rule 的语义：`firmware_native_component` 的 transitive source 进入 action key；`firmware_lib_component` archive 会被收集为输入，但在 SDK Make 显式消费 `H2_BAZEL_ARCHIVES` 之前不会注入最终链接。

## PAL core provider（br23）

`native_component_src/jieli/br23/h2_pal_core` 为 AC695N 实现 Memory、Log、Time、Sync、Queue、Task、Timer 与 Firmware Info 的 PAL provider：

- Provider 只依赖 `h2_jieli_br23_sdk_port.h` 这一层最小 SDK 接口（SDK heap、`put_buf` 调试串口、`timer_get_ms`、`os_time_dly`、`os_mutex_*`/`os_sem_*`、`os_task_create`、`sys_timer_add`/`sys_timeout_add`），`src/h2_jieli_br23_sdk_port.c` 是唯一 include SDK 头文件的翻译单元，由 `jieli_firmware` 的 native 构建编译；host 测试链接 `tests/` 下的确定性 fake，`bazel test //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 在任意 host 运行。
- Time 用 32 位 `timer_get_ms` 扩展为 64 位单调时间，wall time 不支持；Sync 提供 mutex（非递归）与 counting semaphore，condition variable 返回 `H2_PAL_ERR_UNSUPPORTED`；Queue 是 SDK mutex + 两个 semaphore 守护的堆环形缓冲，支持超时、`send_latest` 合并与 `close` 唤醒；Task `start` 走 `os_task_create`（任务返回后 park），`join` 不支持；Timer 用 `sys_timer_add`（周期）与 `sys_timeout_add`（一次性）——SDK 把回调派发到**注册该定时器的任务**上，`sys_timer_del` 不撤回已入队的回调，因此 timer 归属**第一次 start() 的任务**（其它任务再 `start()`/`reset()` 返回 `H2_PAL_ERR_INVALID_STATE`，归属不可转移）：`destroy()` 必须在该任务上调用（按 `xTaskGetCurrentTaskHandle()` 句柄判定，任务名可能重名；其它任务调用返回 `H2_PAL_ERR_INVALID_STATE`），SDK timeout 槽用尽时返回 `H2_PAL_ERR_UNAVAILABLE` 并保留 timer 供重试而不释放，它只置 `destroyed` 标志并在同一任务上注册一次性 reclaim timeout 释放存储，已入队的回调看到标志后直接返回；Firmware Info 报告 wrapper 注入的 `H2_JIELI_FIRMWARE_VERSION`。
- os_api 的 pend 超时以 tick 计且 0 表示永久等待，PAL 的 0 表示不等待，因此 port 用 `os_*_accept` 实现非阻塞尝试，并按 10 ms tick 换算毫秒。

## PAL core provider（wl82）

`native_component_src/jieli/wl82/h2_pal_core` 为 AC791N 实现 Memory、Log、Time、Timer、Sync、Queue、Task、System Event 与 Firmware Info 的 PAL provider，公开入口是 `include/h2_jieli_wl82_platform_core.h`：

- **SDK 边界。** Provider 只依赖 `h2_jieli_wl82_sdk_port.h`（SDK heap、调试输出、board 提供的 64 位单调时钟、`os_time_dly`、`os_mutex_*`/`os_sem_*`、`os_task_create`/删除/park/当前任务、`sys_timer` 派发，以及异常捕获用的单次 `testset` 字节锁）。`src/h2_jieli_wl82_sdk_port.c` 是唯一 include SDK 头文件的翻译单元，只由 `jieli_firmware` native 构建编译（`h2_pal_core` target 为 `manual`）。它在链接时需要 board layout 提供的 `task_info_table` 与 `h2_jieli_default_task_policy`；AC791N DevKit BSP 位于 `boards/jieli_ac791n_devkit/ac791n`；板级实现、task policy 生成、linker export guard 与 firmware link 验收不属于 `h2_pal_core` package。零超时的 mutex/semaphore 用 `os_*_accept`，因为 SDK `pend(…, 0)` 表示永久等待。
- **与 br23 的能力差异。**
  - Sync 支持递归 mutex 和 condition variable。condition 为每个 wait 使用独立 SDK semaphore，只接受非递归 mutex，仍有等待者时 destroy 返回 `H2_PAL_ERR_INVALID_STATE`。
  - Task 支持 `join`：每个任务有唯一 native 名 `<policy>/<hex id>`（匿名任务为 `$h2anon/…`，调用者不能使用该前缀），join 等待完成后按该名字删除；同名 policy 的多个任务互不影响。
  - Timer 的所有生命周期操作同步派发到 SDK `sys_timer` 任务，timer 回调内调用直接内联执行；不支持 ISR 和调度器启动前调用；资源失败保持 timer 停止且可重试。
  - Mutex、semaphore、condition 和 queue 遵循 `config->allocator`，对象从创建到销毁一直持有该 allocator。
- **System Event。** 固定 `H2_PAL_SYSTEM_EVENT_TYPE_COUNT + 8` 个订阅槽位，通过 SDK `sys_event` 异步派发；每条消息为 32 字节 envelope（64 位 generation ceiling、时间戳、source ID、type、flags、payload 长度与 8 字节数据区），使用 `type=0x0100`、`from=0x50`，不会超过 SDK 的 32 字节读取缓冲区。
  - Port 创建常驻 `h2_sysevt` 任务（priority 20、1024 stack words、32 queue words），由该任务注册 handler 并循环 `os_taskq_pend`；SDK 在注册任务的 pend 内执行 `Q_CALLBACK`，所有 PAL handler 按 SDK 入队顺序串行执行。首次启动最多等待 1000 ms，失败返回 `H2_PAL_ERR_TASK`；后续 init 周期复用任务与注册。
  - post 复制 payload：不超过 8 字节内联，9..1024 字节使用 SDK heap 副本，超过 1024 字节返回 `H2_PAL_ERR_INVALID_ARG`，分配失败返回 `H2_PAL_ERR_NO_MEMORY`；post 返回后不再引用调用者内存，副本在整条事件派发完毕或丢弃时释放，handler 的 payload 仅在回调期间有效。
  - 最多保留 4 条尚未完成派发的 PAL 事件；深度耗尽或 SDK ring 返回 -12 时 post 返回 `H2_PAL_ERR_FULL`，并增加只读的 `h2_jieli_wl82_platform_system_event_overflow_count()`，该计数不随 init/deinit 清零。四条消息连同 SDK header 共占 144 字节，SDK 的 256 字节 ring 还与 key、device、network、Bluetooth 等事件共享，因此未达到 PAL 深度上限也可能 FULL。
  - post 的 `timeout_ms` 只约束注册表锁获取；成功表示入队，不传递 handler 返回值，没有匹配订阅时直接返回 OK 而不触碰 SDK。SDK 的 notify 支持中断上下文，但 PAL 的锁、64 位 generation 和 heap 复制不支持，中断 post 返回 `H2_PAL_ERR_INVALID_STATE`，不入队也不增加 overflow。
  - 每次成功 init 获得一个 owner，最多 16383 个，超出返回 `H2_PAL_ERR_FULL`；最后一个 deinit 关闭准入，之后 post/subscribe 返回 `H2_PAL_ERR_INVALID_STATE`，INITIALIZING/CLOSING 期间 init 返回 `H2_PAL_ERR_BUSY`。已排队事件持有操作引用，仍向准入时且尚未退订的订阅交付，即使某个 handler 释放最后一个 owner；最后一个引用释放后才销毁注册表和锁。
  - unsubscribe 先停止准入；外部任务等待该订阅排队事件被消费且运行中的回调返回（包括 CLOSING 阶段），返回后可释放 `handler_user`。dispatcher 内对自身或其他订阅的 unsubscribe 都不等待，以免阻塞唯一的消费任务；已退订的排队回调被跳过，槽位待 queued、dispatching、waiters 全部归零后复用，已准入回调的 user 必须保持有效。
  - 订阅 generation 为 64 位且不回绕：post 在锁内记录 ceiling，入队准入后新建的订阅不会收到该事件，无论由其他任务还是 handler 创建；到达 `UINT64_MAX` 后 subscribe 返回 `H2_PAL_ERR_FULL`，只有完整 teardown 后重新 init 才重置。
  - SDK `sys_event` 任务等待整条 handler 链完成，默认超时 40 s，超时触发 assert 或系统复位；handler 必须保持短小、非阻塞。
- **原子操作。** wl82 的 pi32v2 clang 把 C11/GCC 原子操作降级为 `__sync_*` libcall，而工具链 compiler-rt 的实现在双核上不安全。`h2_jieli_wl82_sdk_port.c` 为 1/2/4/8 字节 `__sync_*` 提供由 SDK spinlock 保护的强定义。
- **Host 验证。** 测试链接 `tests/` 下的确定性 fake 或 pthread SDK port：`bazel test //native_component_src/jieli/wl82/...`，以及 `//tools/bazel:jieli_{allocator,dynamic_task,event_generation,log_text,runtime_events,sdk_memory,sys_event_port,task_identity,wl82_sync_atomics}_test`。`test_jieli_event_generation.py`、`test_jieli_runtime_events.py` 与 `test_jieli_sys_event_port.py` 读取 `CC` 和 `JIELI_TEST_CFLAGS`，可用 GCC 与 TSan 运行。这些测试不替代 AC791N 实机验收。

## Repository-owned native project

AC695N、AC707N 与 AC791N 的 `compile_only` layout 直接拥有 `project.mk`、`app_config.h`、TASK policy、系列所需的 interrupt 配置与最小 board composition；具体 firmware launcher 自己提供 `app_main`。Runner 在 invocation-local SDK 根执行 `make -f <layout>/project.mk h2_link`，但 project 只选择 SDK 的 CPU/common substrate、headers、archives、linker inputs 与 post-build inputs，不编译 `apps/soundbox/**`、`apps/demo/**` 或其它 SDK application project。`tools/bazel/jieli/h2_project_rules.mk` 只提供 Bazel native object/archive 的通用追加规则，不 include SDK Makefile。每个 `firmware_native_component` 源文件使用 layout project 的 flags 与 Bazel include roots 编译到 `$(BUILD_DIR)/h2_bazel/`，`firmware_lib_component` archive 以 link group 进入同一条 `lto-wrapper` 链接。Bazel archive 是非 LTO ELF object，SDK source object 是 LTO bitcode。

AC791N `compile_only/project.mk` 支持调用方提供 `H2_JIELI_LAYOUT_ROOT`，并通过以下 hooks 扩展 reference build；不设置时保持原有配置：

- `H2_JIELI_BOARD_DEFINES`、`H2_JIELI_BOARD_INCLUDES`、`H2_JIELI_BOARD_C_SRC_FILES` 分别追加到 `DEFINES`、`INCLUDES`、`c_SRC_FILES`；`H2_JIELI_BOARD_LIBS` 追加到 `LFLAGS` 的 `--start-group` / `--end-group` 内。
- `H2_JIELI_SDRAM_ENABLE=1` 使用 `-DH2_JIELI_SDRAM_ENABLE=1`，默认仍使用 `-DCONFIG_NO_SDRAM_ENABLE`。
- `H2_JIELI_RESERVED_EXPAND_CONFIG_FILE` 非空时，`pre_build` 将该文件的配置行插入生成的 `isd_config.ini` 中 `[BURNER_CONFIG]` 之前。

## Target task policy

`tools/bazel/jieli_task_policy.bzl` 提供 `jieli_target_task_policy(name, graph, policies, sdk_policies, default_policy, deps = [], tags = [])`，生成 SDK `task_info_table` 并审计 graph 中声明的 PAL 任务。每行格式为 `任务名 优先级 栈word数 队列word数`（栈的 word 为 4 字节），`policies` 覆盖 PAL 任务，`sdk_policies` 注册直接通过 SDK 创建的任务。SDK 的 `#C0` / `#C1` 核绑定前缀保留在生成表中，审计使用去掉前缀的逻辑任务名。

`default_policy` 格式为 `优先级 栈word数 队列word数`，提供动态命名任务的默认预算；静态声明的任务仍须显式配置。生成器拒绝重复名称、非法名称、保留的 `$h2anon/` 名称及越界预算。

## Portable libraries 与离线诊断

FDK AAC 与 Linux FDK AAC decoder provider 允许 pi32v2；`libs/fdk_aac` 在该平台设置 `H2_FDK_EMBEDDED_NO_STDIO=1`，repository patch 将 FDK 的格式化 stdio / getchar 包装替换为空实现，不影响其他平台。LVGL 两个 library 允许 AC791N。tinyh264 使用标准 `<string.h>` 替代 `<memory.h>`，该 patch 应用于所有平台；provider 的 pi32v2 compatibility 保持原有门控。

`tools/bazel/jieli_decode_coredump.py` 用于离线解码 H2CORE v2 record 或完整 flash image，支持 `--offset` 和 `--show-log`；`--retained-ring` 解码 RAM retained log ring，offset 需由对应 ELF 确定。H2CORE 校验记录尺寸、版本、commit marker 与 checksum；retained ring 是 best-effort 日志快照，不代表日志提及的操作已经完成。

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

### AC791N DevKit BSP

`boards/jieli_ac791n_devkit/ac791n` 提供 board identity 与 device UID、TIMER5 单调时钟、UART console、NOR disk partition、NOR 上基于 LittleFS 的 Pref、共享引用计数 NOR write window、SD filesystem、Net/Netif 与 Wi-Fi（STA、AP、saved settings）、BLE host、Audio、Display/Touch/Button input，以及 board runtime config。

所有 NOR erase/write 都必须通过 `h2_jieli_flash_window_open()` / `h2_jieli_flash_window_close()` 配对管理；包括失败在内的每条退出路径都关闭 lease，最后一次 close 恢复 SDK write protection。嵌套或重叠 lease 始终保留首次 open 保存的 protection 值，直到最后一个 lease 关闭。

Board 不安装任何 `NULL` vtable member；未实现的 operation 返回 `H2_PAL_ERR_UNSUPPORTED`，包括 `tcp_listen`、`tcp_accept`、`set_default`，以及通过共享 `h2_pal_unsupported_ble_*` stub 填充的 BLE scan / central-GATT entry。

Wi-Fi scan 要求当前处于 STA mode 且已关联：非 STA mode 返回 `H2_PAL_ERR_INVALID_STATE`，未关联时 SDK 拒绝请求并返回 `H2_PAL_ERR_BUSY`。scan 返回 `H2_PAL_ERR_TIMEOUT` 后，status 恢复报告关联状态，不再报告 `SCANNING`，但 scan 仍由 SDK 持有，新的 scan / connect 返回 `H2_PAL_ERR_BUSY`，直到迟到的完成事件被回收，或通过 `sta_disconnect` / `ap_stop` / `ap_start` 的停止路径复位；只有 `wifi_off()` 成功后才释放 scan 所有权，正在等待完成或清理中的 scan 仍拒绝停止操作。

`h2_jieli_ac791n_devkit_runtime_config()` 只提供 `h2_runtime_config_t`，`h2_jieli_ac791n_devkit_runtime_deinit()` 只释放 board 持有的 SD filesystem 资源。完整 Runtime 初始化、input 启动、app 调用和 `h2_runtime_deinit()` 属于最终 artifact target，见 [Runtime 初始化与接线](../runtime.md#初始化与接线)。

## 烧录与升级边界

- 空片首次烧录不在 GizOS 范围：需要杰理强制升级工具（Windows，芯片 USB 枚举为 UBOOT 设备）或一拖二烧写器（AC791N 带 KEY 烧录仅支持一拖二）。
- 日常迭代用 `update.ufw`：设备虚拟 U 盘拷贝、SD 卡升级或 HTTP/FTP OTA。
- `jl_isd.bin` 是完整 flash 镜像，供烧写器或有 KEY 的首刷流程使用。

## Validation

- `bazel test --config=macos_arm64 //tools/bazel:jieli_runner_test //tools/bazel:jieli_post_wl82_test //tools/bazel:jieli_decode_coredump_test //tools/bazel:jieli_task_policy_test //tools/bazel:jieli_compile_only_project_test` 验证 runner、离线打包、解码、task policy 与 Make hooks；Linux host 使用对应 host config。
- `bazel test --config=macos_arm64 //tools/bazel/tests/jieli_task_policy:generated_policy_test //tools/bazel/tests/jieli_task_policy:missing_policy_test //tools/bazel/tests/jieli_task_policy:undeclared_policy_test` 实际加载 task policy 宏，验证生成 C、audit task list，以及缺失策略和未声明任务的 analysis failure；Linux host 使用对应 host config。
- `bazel test //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 验证 BR23 PAL core。
- `bazel test //native_component_src/jieli/wl82/...` 在 Linux/macOS host 运行 wl82 PAL core 测试。
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
