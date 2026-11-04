# 项目结构

本文只定义 H2Loader 源码目录及其 ownership。固件运行时角色和分区约束见 [固件结构分区与类型](./firmware_types)，安装和启动状态见 [更新、启动与回退](./update/)。

## 项目布局

```text
libs/h2loader/
├── host/                                # Host commands, package/flash clients and lifecycle
├── device/                              # Device core, common service and serial/KCP engine
└── web/                                 # Browser JS/runtime/WASM SDK

projects/h2loader/
├── apps/loader/app/                      # Portable Loader App
├── tools/bazel/                          # Firmware layout, package and recovery rules
└── targets/
    ├── loader/<manufacturer>/<board>/   # Native firmware and both package formats
    └── npm_package/h2loader/             # @gizclaw/h2loader artifact

projects/e2e/apps/<test-app>/app/         # 共享 target-independent E2E 测试 App
projects/<group>/apps/<app>/app/           # 其它产品拥有的 portable App
projects/<owner>/targets/<app>/<manufacturer>/<board>/ # Owner 的 H2Loader-managed package
boards/<company>/<board>/<target>/                   # 物理 board BSP、defaults 与 firmware layouts
native_component_src/<target>/             # 跨产品原生 SDK component source
libs/h2loader/host/                        # Portable Host Core
tools/gizloader/cli/                      # Commands, native entry, OS adapters and tests
tools/gizloader/native/                   # Gizloader CLI/session sidecar composition
projects/e2e/libs/h2loader_runner/         # Device E2E orchestration
projects/e2e/targets/cc_binary/e2e-runner/  # Native E2E process entry
```

## 当前 Loader 身份

`h2_loader_pal_current_image_identity()` reads the running partition through
Power and hashes the explicitly configured Image view's known length. Stored
artifact and native executable lengths remain separate; ESP startup explicitly
selects its native length view to preserve the canonical running-image identity.
Unknown exact length returns `UNSUPPORTED`; capacity is never silently treated
as an exact image length. Compatibility composition can explicitly use
`h2_loader_read_current_loader_identity()` with matching H2 metadata or its
bounded primary-slot fallback policy. Native code length, stored artifact length
and UFW shadow length remain distinct.

Loader/App composition reads stable device bytes through Device Identity PAL
and uses `h2_loader_pal_device_uid()` for the existing twelve lowercase hex
digits. ESP32-P4 uses its local factory identity, and JieLi preserves the
persisted controller-address byte ordering. Borrowed providers and shared digest
state remain valid throughout each caller-serialized operation.

## App 入口

目录位置：`projects/h2loader/apps/`

`apps/loader/app/` 保存 portable Loader App。它组织设备端 command、安装和启动流程，但不包含 board、partition label、SDK startup 或 Host UI。

The Batch Loader UI belongs to `GizClaw/www`. Shared browser bindings in
`libs/h2loader/web` expose the public Promise API over Host Core and Web PAL.
`projects/h2loader/targets/npm_package/h2loader` packages matching
JS/runtime/WASM as `@gizclaw/h2loader`; consumers do not use the raw Emscripten
ABI.

`tools/gizloader/cli/` owns the C11 CLI implementation, argument validation, command dispatch, stable output, native process entry, OS adapters and regression tests. It consumes Runtime, PAL, `libs/command/` and `libs/h2loader/host/`. `tools/gizloader/native/` composes the CLI/session sidecars. The canonical public command is `gizloader`.

`projects/e2e/apps/<test-app>/app/` owns cross-target test Apps; product Apps remain in their own project groups. H2Loader installation does not change App or artifact ownership.

各 App owner 在自己的 `targets/<app>/<manufacturer>/<board>/` 中提供 Runtime、App command/confirmation、package identity、board wiring 和最终 package。它们可以依赖 H2Loader library、native component 与 packaging rule，但这些 adapter dependency 不把 artifact ownership 转移给 H2Loader。

## H2Loader 共用代码

目录位置：`libs/h2loader/device/`

This shared Device library contains stable APIs and portable implementations
used by Loader firmware and managed App firmware, including:

- Package、image identity 与安装状态。
- SDK-independent SHA-256 streaming helper，由 Loader/App 的 digest adapter 共用，不从其它 firmware entry 拉取源码。
- App Stage 收尾、`reboot app|loader|upgrade` 与 return-to-loader。
- Loader/App boot intent。
- Device command handler 与结构化 status/stats。

共用代码不包含 Runtime 初始化、BSP wiring、SDK object、具体 partition label、Host transport backend 或 App 业务。

[API Reference](/references/h2loader)

## PAL and device services

The device dependency is H2Loader application/protocol code → public PAL
contracts → platform providers → vendor SDK. `libs/h2loader/device` contains
both the core and one common Loader/App service with a shared serial/KCP
engine. Timing, admission, allocator, package-entry and native-evidence needs
are injected capabilities/configuration. Legacy persistence codecs preserve
existing records without creating platform-specific service libraries.

- Image PAL owns native image access and the write/finish/abort transaction. Its
  stored byte view is the exact artifact verification domain: ESP image bytes,
  BK encoded physical bytes, or JieLi UFW shadow bytes.
- Power PAL owns native boot state, preparation, confirmation and selection.
  Finish leaves a prepared candidate; destination verification and H2 metadata
  commits precede preparation and selection. Evidence returned by a provider is
  caller-owned; H2 retains its checksum binding and persistence policy.
- Device Identity and Mem PAL expose stable device bytes and real heap-region
  observations. Unsupported observations remain explicit.
- Stream setup, frame/log arbitration, synchronization, allocation, timing and
  hardware access use PAL. SHA-256 uses the shared portable implementation, with
  positive PAL scheduling delays where watchdog cooperation is required.
- Cache/IRAM, Flash encryption, AP/CP handoff, native headers and early startup
  mechanisms belong to platform providers under top-level
  `native_component_src/`.
- Physical wiring and layouts belong to `boards/`; targets compose capabilities
  and retain minimal SDK startup/component registration.
- Host CLI and OS adapters belong to `tools/gizloader/cli`; E2E orchestration,
  firmware fixtures and targets belong to `projects/e2e`.

Generic providers do not import H2Loader role, manifest, Stage, checksum binding
or preference-policy types. The project has no separate native business-code
ownership exemption. The repository's `libs/h2loader/README.md` describes the
shared contracts and lifecycle ordering; `projects/h2loader/README.md` describes
firmware and artifact composition.

## 固件构建入口

目录位置：`projects/<owner>/targets/`

每个 `projects/<owner>/targets/<app>/<manufacturer>/<board>/` 都是一个可独立构建的 H2Loader package 入口。`<owner>` 是真实 App owner；`h2loader_tar_zlib` 只表示安装产物类型。Entry 同时知道 portable App 与物理 board，因此负责：

1. 选择 BSP 和 target build config。
2. 取得 board Runtime config。
3. 提供 App `component_id` 到 board `periph_id` 的 mapping。
4. 映射当前 firmware 使用的 partition 与 mount。
5. 初始化 Runtime。
6. 组装 App public config 并调用阻塞式 App entry。
7. App 返回后释放 Runtime 并执行 reboot 或平台退出策略。

Entry 必须保持薄。Package parser、安装状态机、PAL backend 和可复用 App 逻辑不能复制进 entry。

每个 entry 的 `BUILD.bazel` 是 repository-owned build graph 的唯一入口：一个
package-local `firmware_native_component` launcher 依赖真实 App、Board、SDK component
和 graph-reachable portable archive。Launcher 的 `srcs`、`hdrs`、`data` 和 firmware
project 输入必须逐文件声明，不得使用 recursive glob，也不得把整个 Board、App 或
Project root 当成 action input。CMake 文件仍作为原生 SDK 必需输入存在；entry 不保留
image-local SDK config。GizOS 仓库内的 H2Loader wrapper 根据 entry 声明的
`target` 与 `board` 从 `boards/<company>/<board>/<target>/layouts/` 注入 partition、layout
defaults 和 recovery 配置。通过 Bzlmod 消费 GizOS 的下游仓库可以保留自己的私有
Board，并通过 `layout_files` 显式传入该仓库拥有的完整 layout labels；私有 Board
不得加入 GizOS 的内建 Board registry，也不得复制 H2Loader wrapper。最终 SDK 配置
按具名 target-owned profile、board defaults 和 layout defaults 的顺序合并。内建
DevKit 要求每个 `h2loader_esp_idf_firmware` 调用方显式选择 `usb` 或 `uart` console；
wrapper 把对应 defaults file 作为 action input，并用 target-owned CMake definition
传递 profile 文件名。CMake 只消费 Bazel 选择的 profile 和 runner 生成的
component/source manifest，不接受调用方用 ambient cache value 改写 profile，也不再
维护第二份 first-party component path 或 source inventory。

一个 portable App 只有一份实现；每个支持它的 board 仍需要单独 variant，因为 BSP、partition、component mapping 与 SDK build root 不同。

ESP-IDF entry 的 CMake project 仍拥有 component discovery、Kconfig、linker、partition 和 bootloader 语义。内部 `:firmware` 使用 `h2loader_esp_idf_firmware` 按 `target` 与 `board` 选择 native layout，再委托通用 `esp_idf_firmware` 在 action-owned tree 内调用原生 `idf.py build`。它只暴露结构化 ELF、map、app、bootloader、partition table、由同一次构建的 flash arguments 生成并从 `0x0` 烧录的 `combined_factory.bin`、flash metadata 和完整 flash file set；Loader recovery 由外层 package rule 生成。公开交付从 `bazel build --config=<esp32s3|esp32p4> //projects/<owner>/targets/<app>/<manufacturer>/<board>:package` 进入；`:package` 使用平台无关的 `h2loader_tar_zlib` 消费 `:firmware`，生成唯一 H2Loader package、recovery bundle、release metadata，并为 ESP Loader 把 combined image 复制成 target-owned `.combined_factory.bin` release asset。Metadata 用 `factory-flash`、`.combined_factory.bin` release suffix 和 offset `0` 描述直接烧录合同。仅供硬件验收的 alternate package 不得标记 `firmware-release`；release discovery 只选择显式 opt-in 的 Loader canonical `:package_v2`，并额外拒绝诊断 identity。现有 `no-release` 仅作诊断标记，不再参与发布查询。Combined image 不取得 managed package identity，也不参与 H2Loader runtime update。调用方不能把 action 内部的 native command 或临时 build tree 当成第二个公开入口。

BK7258 entry 的 CMake project 继续拥有 AP/CP 与 linker 语义。每块 H2Loader-managed BK7258 Board 在自己的 `boards/<company>/<board>/bk7258/` 中拥有 board defaults，并在 `layouts/<layout>/` 中拥有 OTA flash geometry、SMP build config、recovery config、GPIO、RAM region 和 AP/CP layout defaults；同一 Board 的 Loader 与 App 都由 `h2loader_bk7258_firmware` 注入对应 layout，target 目录不保留 SDK config。通过 Bzlmod 使用私有 Board 时，consumer 必须在 `layout_files` 中显式提供 AP/CP config、GPIO、RAM region、project-support 和 Loader layout 的 recovery-config labels。`h2loader_bk7258_firmware` 把 layout 固定的 recovery config 放入 `Bk7258FirmwareInfo`，外层 package rule 只消费 provider 生成 Loader recovery bundle，不按 board 名查找配置，也不接受 package-level override。支持跨 Board OTA 兼容的 Board 必须保持相同 flash geometry，并由 graph test 比较其内容。`ram_regions.csv` 是 image linker RAM profile 而不是 OTA partition layout。通用 `bk7258_firmware` 的结构化 provider 保持 native outputs 和 layout recovery config 的 owner/type 边界。

## 物理 Board 支持

目录位置：`boards/<company>/<board>/<target>/`

这里描述物理 board 的 GPIO、bus、peripheral、memory、BSP wiring 和 `layouts/<profile>/`。Board defaults 表达所有 firmware 共用的硬件条件，layout defaults 表达 `standard`、`h2loader` 等启动与升级模型共用的 partition、SDK config、link 和 recovery 输入；并非所有 image 都使用但能在同一 Board 上复用的 SDK、GPIO 或 memory 差异使用 layout 下可组合的具名 profile。最终 artifact entry 只选择 profile，不拥有 SDK config。Board 不知道某个 App 的业务状态，也不决定 package policy。

## 平台共用组件

目录位置：`native_component_src/<target>/`

This tree contains reusable platform PAL providers and native startup
mechanisms. It owns SDK image/update operations, boot headers, Flash/cache
safety and physical transport drivers. Minimal image startup and capability
composition remain in project targets; physical wiring and geometry remain in
`boards/`.

## Host 工具

The native CLI implementation, process entry, platform adapters and tests are in `tools/gizloader/cli/`. `tools/gizloader/native/` builds the Gizloader CLI/session sidecars. Public device operations use `gizloader`.

The reusable JS/runtime/WASM SDK remains in `libs/h2loader/web/`, its npm artifact in `projects/h2loader/targets/npm_package/h2loader/`, and the Batch Loader UI in `GizClaw/www`. Web SDK and native CLI share authoritative identity, typed commands, package validation and lifecycle verification through `libs/h2loader/host/`.

Device E2E orchestration belongs to `projects/e2e/libs/h2loader_runner/`, with native entry `projects/e2e/targets/cc_binary/e2e-runner/`. Host code runs outside device firmware and does not use device Runtime hardware providers.

## Target-owned task policy

每个 H2Loader-managed firmware target 拥有自己的 task-name policy，shared provider 不持有具体 task name；module 只用 `h2_tasks` 声明它启动的 task name，target 必须为图里每个声明的 task 给出一行 policy。ESP launcher 在 `h2_esp_board_runtime_config()`、`h2_runtime_init()` 或直接 PAL task creation 前调用 `h2_esp_target_task_policy_install()`；BK AP launcher 在 board Runtime configuration 前、BK CP launcher 在 startup task creation 前调用 `h2_bk_target_task_policy_install()`。安装失败必须停止 startup。

Portable task name 统一使用 `{mod}/{task_name}`。`mod` 可以包含多层路径，例如 `h2peer/net/request`。系统全局、library 和 third-party task 使用 `$` 前缀，例如 `$h2peer/net`；App-owned task 不加前缀，例如 `audio-system/music`。名称由实际创建 task 的 library 或 App 以 `const char[]` 导出，target-owned policy 注册必须引用该常量，不能复制字符串字面量。平台或三方库产生的线程名由对应适配层映射到 portable name，例如 LVGL 的 `swdraw` 映射为 `$lvgl/swdraw`。

ESP policy table 写在 concrete target package 的 `esp_target_task_policy` 调用里，BK AP/CP 写在同一 package 的 `bk7258_target_task_policy` 里；两者的 component source 与 host test 都由 `//tools/bazel:target_task_policy.bzl` 生成，源码树里只留 API header 和 component `CMakeLists.txt`。底层 native firmware rule 分别通过 `task_policy` 或 `ap_task_policy`/`cp_task_policy` 接入 component，H2Loader wrapper 负责要求这些参数。Board layout 只保留硬件、SDK、partition、GPIO 与 RAM-region 输入；private task name 因而不会进入 GizOS public board layout。
