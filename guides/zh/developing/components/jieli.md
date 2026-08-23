# JieLi Components

杰理 (JieLi) 芯片按产品线发布独立 SDK（AC695N 蓝牙音频、AC79 WiFi+BT 等），但共用一套私有 LLVM/Clang 工具链与 Linux post-build 打包工具。GizOS 用**一条工具链 repository、一个 `jieli_firmware` external rule、按系列区分的 SDK locator 与 platform** 接入它们：`target = br23` 对应 AC695N（Lucky Kitty 玄学机的 AC6956C），`target = wl82` 对应 AC791N（AC79 开发板）。本文定义该 rule 的 contract、执行边界与烧录/升级边界，以及 `native_component_src/jieli/br23/h2_pal_core` 的 PAL core provider。

## 工具链形态

- 杰理工具链是私有 clang 4.0.1（`-target pi32v2 -mcpu=r3` 或 `-target q32s`），仅发布 Windows 与 Linux x86_64 预编译二进制；一个 tarball 同时包含 `common/`、`pi32/`、`pi32v2/`、`q32s/`。AC695N/AC696N/AC701N/AC791N 都使用 `pi32v2/bin`，AC63/AW31N 使用 `q32s/bin`。
- post-build 工具（`isd_download`、`fw_add`、`ufw_maker`、`packres`、`remove_tailing_zeros` 等）是 Linux x86_64 Qt 程序，必须以 `QT_QPA_PLATFORM=offscreen` 运行。
- 上游下载短链每次重定向到最新版本且不保留历史，因此 tarball 由 firmwares-devenv 镜像（`tools/jieli_toolchain/archives`）并由 `make jieli-toolchain` 解包到 `.tools/jieli-linux-toolchains-20250805.1`、`.tools/jieli-linux-post-build-tools-20260728.1`（`--strip-components=1`，`packres` 改名 `pack_res`）；`tools/bazel/native_versions/jieli_toolchain_archives.txt` 固定 archive SHA-256 与 bootstrap 产出的 expanded-tree SHA-256，`@h2_jieli_toolchain` 与 `@h2_jieli_postbuild` repository 只消费 `JIELI_TOOLCHAIN_ROOT`、`JIELI_POSTBUILD_ROOT` 指向的解包树并用 `toolchain_identity.py` 校验，不从网络下载。CI 用 deploy key sparse-checkout firmware-devenv 的 `tools/jieli_toolchain` 并运行同一个 `bootstrap.sh`。
- 链接需要 `ulimit -n >= 8192`；runner 在调用 Make 前提升该限制并 fail closed。

## pi32v2 compatible graph

`--config=ac695n` / `--config=ac791n` 构建完整 compatible graph：仓库里每个没有平台门控的 `cc_library` 都会用 pi32v2 toolchain 编译。clang 4.0.1 对 `{0}` 聚合初始化的误报由 toolchain 的 `unfiltered_compile_flags` 在每个 target 的 copts 之后统一关闭；真正编不了的目标用 `PI32V2_UNSUPPORTED_ARTIFACT_COMPATIBILITY`（`tools/bazel/platforms/compatibility.bzl`）显式退出 pi32v2 graph，并在 BUILD 注释里写明原因，而不是静默跳过。当前退出的有：`//libs/game_runtime` 的四个 C++17 target（clang 4.0.1 无 `-std=c++17`，pixa_games 与 H106 main 随之退出）、`//libs/pal/providers/libco`（无 pi32v2 上下文切换汇编、`const _Atomic` 操作数被拒）、`//libs/pal/providers/tinyh264`（vendor 依赖 `<memory.h>`）、`//projects/h106/apps/mfg/app:mfg`（`const _Atomic` 加载）、`//projects/e2e/apps/gizclaw/app:gizclaw_e2e` 与 `//tools/bazel:vendor_third_party_opus`（`opus_multistream_encoder.c` 触发 clang 4.0.1 后端崩溃，gizclaw/audio-system 随之退出）。工具链升级后应收回这些标记。

## SDK locator

| 系列 | 变量 | 指向 | 验证 |
| --- | --- | --- | --- |
| AC695N (`br23`) | `JIELI_AC695N_SDK_PATH` | `h2vivi/AC695N_Soundbox_SDK` checkout 内的 SDK 根目录 `…/jieli_ac695n_sdk/SDK`（firmware-devenv 导出） | 所属 Git checkout 的 `native_versions/jieli_ac695n_sdk_commit.txt` exact commit、tracked cleanliness、`Makefile` 与 `cpu/br23/tools/download.c` 存在 |
| AC791N (`wl82`) | `JIELI_AC791N_SDK_PATH` | `h2vivi/fw-AC791N_SDK` checkout 根目录（即 SDK 根） | `native_versions/jieli_ac791n_sdk_commit.txt`、cleanliness、`apps/demo/demo_hello/board/wl82/Makefile` 存在 |

两个 SDK 都是杰理代理商云芯 (`gitcode.com/yunthinker`) 发布仓的 h2vivi 私有全量镜像（含全部 refs 与 LFS）。`fw-AC791N_SDK` 用 Git LFS 托管二进制，执行构建的 host（CI runner 或 dev container）必须安装 `git-lfs`，否则 runner 的 cleanliness 检查会把 LFS 文件判为已修改并 fail closed。`.bazelrc` 的 `--config=ac695n` / `--config=ac791n` 只用 `--repo_env` 把对应变量交给 repository rule；变量未设置时 repository 注册为 disabled locator，firmware target 因 compatibility 被跳过，不阻塞无关 graph。

## Bazel external rule

`jieli_firmware(name, target, board, image, sdk_project, sdk_elf, graph, srcs)`：

- `target` 只能是 `br23` 或 `wl82`；macro 按 target 绑定 SDK locator、commit 文件、本地 post 脚本（`tools/bazel/jieli/local_post_<target>.sh`）与 SDK 子目录，调用方不得覆盖。
- `sdk_project` 是 SDK 根相对的 Makefile 目录（AC695N 为 `.`，AC791N demo 为 `apps/demo/demo_hello/board/wl82`）；`sdk_elf` 必须与该 Makefile 的 `$(OUT_ELF)` 拼写完全一致，runner 只驱动 `pre_build` 与该 ELF 两个 Make target，跳过官方 `all` 里走 host-client 云打包的 `download.sh`。
- Runner 把 SDK 子树复制到 invocation-local 目录（排除 `.git`、`doc`、`ui_project`），用 `TOOL_DIR=<pi32v2/bin>` 覆盖 Makefile 的 `/opt/jieli` 默认值，再执行仓库自有 post 脚本：objcopy 抽取官方 `download.c` 列出的段拼成 `app.bin`，`isd_download -tonorflash -dev <target> …` 本地产出 `jl_isd.bin`/`jl_isd.fw`（无设备时的 "Device Offline" 退出按成功处理，产物非空另行校验），`fw_add` 附加 OTA 与 `script.ver`，`ufw_maker -fw_to_ufw` 产出升级包。AC695N 使用 `download/standard/` 内随 SDK 提供的 `AC69XX.key`。
- 固定输出 `firmware/firmware.elf`、`symbols.txt`（objsizedump 符号表）、`jl_isd.bin`（完整 NOR flash 镜像）、`jl_isd.fw`、`update.ufw`（USB 虚拟盘 / SD 卡 / OTA 升级包）与 `manifest.json`；`JieliFirmwareInfo`、`DefaultInfo.files` 与 `OutputGroupInfo.release` 暴露相同文件。不返回 `FirmwareReleaseInfo`，不进入 H2Loader package 或 GitHub Release matrix。
- Action 在当前 runner 上 unsandboxed、non-remote-exec 执行，不设置 `local`，声明 4 CPU / 4 GiB；只读取 allowlist environment（fixed PATH、`QT_QPA_PLATFORM=offscreen`、invocation-local `HOME`/`TMPDIR`），不继承 caller `PATH`。成功结果进入 local/GCS action cache。Action 不执行 flash、串口或设备操作。
- **执行平台只能是 Linux x86_64。** Rule 的 compatibility 同时要求 `h2_firmware_target` 与 `h2_host_os=linux`，macOS host 上 `bazel build --config=ac695n //...` 把 firmware target 标为 incompatible 并跳过；macOS 开发者在 Linux dev container 内运行整个 Bazel。Runner 自身也在触碰 SDK 前拒绝非 Linux x86_64 host。不在 Bazel action 内包装 `docker run`。
- `graph` 与 `srcs` 沿用其他 external rule 的语义：`firmware_native_component` 的 transitive source 进入 action key；`firmware_lib_component` archive 会被收集为输入，但在 SDK Make 显式消费 `H2_BAZEL_ARCHIVES` 之前不会注入最终链接。

## PAL core provider（br23）

`native_component_src/jieli/br23/h2_pal_core` 为 AC695N 实现 Memory、Log、Time、Sync、Queue、Task、Timer 与 Firmware Info 的 PAL provider：

- Provider 只依赖 `h2_jieli_br23_sdk_port.h` 这一层最小 SDK 接口（SDK heap、`put_buf` 调试串口、`timer_get_ms`、`os_time_dly`、`os_mutex_*`/`os_sem_*`、`os_task_create`、`sys_timer_add`/`sys_timeout_add`），`src/h2_jieli_br23_sdk_port.c` 是唯一 include SDK 头文件的翻译单元，由 `jieli_firmware` 的 native 构建编译；host 测试链接 `tests/` 下的确定性 fake，`bazel test //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 在任意 host 运行。
- Time 用 32 位 `timer_get_ms` 扩展为 64 位单调时间，wall time 不支持；Sync 提供 mutex（非递归）与 counting semaphore，condition variable 返回 `H2_PAL_ERR_UNSUPPORTED`；Queue 是 SDK mutex + 两个 semaphore 守护的堆环形缓冲，支持超时、`send_latest` 合并与 `close` 唤醒；Task `start` 走 `os_task_create`（任务返回后 park），`join` 不支持；Timer 用 `sys_timer_add`（周期）与 `sys_timeout_add`（一次性）——SDK 把回调派发到**注册该定时器的任务**上，`sys_timer_del` 不撤回已入队的回调，因此 timer 归属**第一次 start() 的任务**（其它任务再 `start()`/`reset()` 返回 `H2_PAL_ERR_INVALID_STATE`，归属不可转移）：`destroy()` 必须在该任务上调用（按 `xTaskGetCurrentTaskHandle()` 句柄判定，任务名可能重名；其它任务调用返回 `H2_PAL_ERR_INVALID_STATE`），SDK timeout 槽用尽时返回 `H2_PAL_ERR_UNAVAILABLE` 并保留 timer 供重试而不释放，它只置 `destroyed` 标志并在同一任务上注册一次性 reclaim timeout 释放存储，已入队的回调看到标志后直接返回；Firmware Info 报告 wrapper 注入的 `H2_JIELI_FIRMWARE_VERSION`。
- os_api 的 pend 超时以 tick 计且 0 表示永久等待，PAL 的 0 表示不等待，因此 port 用 `os_*_accept` 实现非阻塞尝试，并按 10 ms tick 换算毫秒。

## SDK link wrapper

Runner 不直接调用 SDK 的 `all`（其中的 `download.sh` 走 host-client 云打包），而是从 SDK 项目目录执行 `make -f tools/bazel/jieli/h2_sdk_wrapper.mk h2_link`。wrapper 先 `include Makefile` 取得 SDK 自己的 `CFLAGS/DEFINES/INCLUDES/OBJS/LD/LFLAGS/LIBS/OUT_ELF`，再 include runner 生成的 `h2_bazel_components.mk`（`H2_BAZEL_NATIVE_SRCS`、`H2_BAZEL_NATIVE_INCLUDES`、`H2_BAZEL_ARCHIVES`、`H2_BAZEL_DEFINES`、`H2_BAZEL_SDK_ENTRY_SOURCE`）：每个 `firmware_native_component` 源文件用 SDK flags + Bazel include roots 编译到 `$(BUILD_DIR)/h2_bazel/`，`firmware_lib_component` 的 archive 以 `--start-group … --end-group` 放在 SDK 对象之后、SDK 库组之前进入同一条 `lto-wrapper` 链接；`sdk_entry_source` 指定的 SDK 应用入口文件（AC695N 为 `apps/soundbox/app_main.c`）仍然编译，但其 `app_main` 被重命名，由 launcher component 提供真正的 `app_main`。Bazel archive 编译为非 LTO ELF 对象（`firmware_lib_component` 用 `nm` 做 ABI 检查），SDK 自身对象为 LTO bitcode，gold plugin 混链。

## 板与 entry

- `boards/lucky_kitty_v1_3/ac6956c/` 拥有 Lucky Kitty 玄学机（AC6956C，原理图 ZC-P26018_C0707 V1.3）的 SDK 无关接线常量与 host 测试，以及由 SDK 编译的 `:board` native component（把调试 UART0 改到 Type-C D+ 测试点 TP20，波特率 1 Mbps）；详见 Lucky Kitty v1.3。
- Firmware entry 只为仓库自己的 portable App 建立，位于 `projects/<project>/targets/jieli_firmware/<image>/<board>/`；不为 SDK 自带 demo 建 entry。首个 entry 是 `//projects/e2e/targets/jieli_firmware/pal/lucky_kitty_v1_3:firmware`：`firmware_lib_component` 聚合 `//libs/pal`、`//libs/pal:unsupported`、`//libs/runtime` 与 `//projects/e2e/apps/pal/app:pal_e2e`，launcher `src/main.c` 提供 `app_main`，用 br23 PAL core + unsupported 填满 Runtime config，运行 PAL E2E core suite 并经 Log 持续汇报 `H2_PAL_E2E_PASS/FAIL` 与逐 case 结果；condition variable 在 br23 上不支持，对应 condition 与 concurrency case 预期失败。

## 烧录与升级边界

- 空片首次烧录不在 GizOS 范围：需要杰理强制升级工具（Windows，芯片 USB 枚举为 UBOOT 设备）或一拖二烧写器（AC791N 带 KEY 烧录仅支持一拖二）。
- 日常迭代用 `update.ufw`：设备虚拟 U 盘拷贝、SD 卡升级或 HTTP/FTP OTA；Lucky Kitty 的 Type-C D+/D- 直连 MCU，可走板载 USB 升级。
- `jl_isd.bin` 是完整 flash 镜像，供烧写器或有 KEY 的首刷流程使用。

## Validation

- `bazel test //tools/bazel:jieli_runner_test //boards/lucky_kitty_v1_3/ac6956c:test_h2_ac6956c_lucky_kitty_config //native_component_src/jieli/br23/h2_pal_core:test_jieli_br23_platform_core` 在任意 host 运行。
- Linux x86_64：`. ../firmwares-devenv/export.sh && bazel build --config=ac695n //projects/e2e/targets/jieli_firmware/pal/lucky_kitty_v1_3:firmware`（或 `//...`）；重复构建应命中 action cache。
- macOS：同一命令应报告 target incompatible 而非失败。
- 真机：空片首刷与 `update.ufw` 升级在开发板到位后各验证一次，记录于对应 board 文档。
