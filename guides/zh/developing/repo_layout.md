# 目录结构

这个文档定义 GizOS 仓库的完整目录结构和 ownership。新增代码时，先确定代码属于仓库级 library、target component、物理 board BSP、project app、固件 launcher、host tool、third-party source、文档、实验代码还是生成产物。

## 目录结构

```text
.
├── AGENTS.md                              # Agent 工作入口和文档索引
├── .gitattributes                        # Git attributes
├── .gitignore                            # Git ignore 规则
├── .bazelversion                         # CI graph 使用的 exact Bazel 版本
├── MODULE.bazel                          # Bzlmod 依赖
├── REPO.bazel                            # 根 query 排除的内嵌 repository
├── BUILD.bazel                           # 仓库级 logical CI graph 入口
├── Makefile                              # 公共 target、默认变量与命令调度
├── scripts/                            # 公共 Make target 的同名脚本入口
│   ├── config/                           # 环境与帮助命令
│   ├── build/                            # Bazel build、plan、CI 与 release 命令
│   ├── test/                             # coverage 与 live E2E 命令
│   ├── guides/                           # Guides build、dev 与 preview 命令
│   ├── web/                              # Web build 与 preview 命令
│   └── common/                           # 两个以上 Make target 共用的实现
│
├── libs/                                 # 仓库级 Bazel libraries
│   ├── pal/
│   │   ├── include/                      # PAL public contract
│   │   ├── src/                          # Canonical unsupported API objects
│   │   ├── tests/                        # PAL contract 和 unsupported API tests
│   │   ├── BUILD.bazel                   # PAL contract build、test 和依赖
│   │   └── providers/                    # PAL 的具名 provider implementations
│   │       ├── <library>/                # Reusable third-party/PAL adapter
│   │       ├── linux/                    # Linux userspace PAL backend
│   │       ├── darwin/                   # Darwin host PAL backend
│   │       ├── posix/                    # OS owner 私下复用的 POSIX source
│   │       ├── allwinner-linux/          # Allwinner Linux vendor integration
│   │       ├── desktop/                  # 跨 Linux/macOS Desktop composition
│   │       ├── ios/                      # iOS PAL backend
│   │       ├── android/                  # Android PAL backend
│   │       └── web/                      # Browser/WASM PAL backend
│   ├── <library>/
│   │   ├── include/                      # Library public API
│   │   ├── src/                          # Portable implementation
│   │   ├── tests/                        # Library-local native tests
│   │   └── BUILD.bazel                   # C/C++ build、test 和显式依赖
│
├── third_party/
│   ├── <dependency>/                     # 必须提交到仓库的少量 vendored source
│   ├── <dependency>.BUILD.bazel          # Bazel vendor overlay
│   └── <dependency>_patch/               # GizOS-owned upstream patch 与新增 source
│
├── native_component_src/                 # 原生 SDK build system 编译的 component source
│   ├── esp-idf6.x/                       # ESP-IDF 6.x family components
│   ├── bk3633/                           # BK3633 SDK components
│   └── bk7258/
│       ├── ap/                           # BK AP reusable components
│       └── cp/                           # BK CP reusable components
│
├── boards/                               # 物理 board BSP
│   └── <board>/
│       └── <chip-or-target>/
│           ├── include/                  # BSP public API
│           ├── src/                      # BSP private implementation
│           ├── layouts/<profile>/        # Standard、H2Loader 等 firmware layout
│           └── <board-config>            # GPIO、SDK defaults 等
│
├── projects/                             # Project-owned App、library、native component source 与最终产物
│   ├── <app>/                            # 独立 portable app project
│   │   ├── app/
│   │   │   ├── include/                  # App public API
│   │   │   └── src/                      # App private implementation
│   │   ├── assets/                       # App assets，可选
│   │   ├── data/                         # App package data，可选
│   │   ├── ia/                           # App information architecture 和产品设计输入，可选
│   │   ├── native_component_src/         # 仅服务该 project 的原生 SDK component source，可选
│   │   └── targets/<artifact-rule>/      # 按最终 Bazel rule 命名的产物 root
│   │       └── <app>[/<variant>]/
│   └── <group>/                          # 多 App project group
│       ├── libs/                         # Project-owned shared libraries
│       │   ├── cmake/                    # Reusable library CMake build API，可选
│       │   └── <library>/
│       │       ├── include/              # Library public API
│       │       ├── src/                  # Portable implementation
│       │       ├── tests/                # Library tests
│       │       └── BUILD.bazel
│       ├── native_component_src/         # Group-local native SDK component source
│       │   └── <platform-or-target>/
│       │       └── <component>/
│       ├── apps/
│       │   └── <app>/
│       │       ├── app/
│       │       │   ├── include/
│       │       │   └── src/
│       │       ├── assets/               # 可选
│       │       └── data/                 # 可选
│       └── targets/<artifact-rule>/
│           └── <app>[/<variant>]/        # 最终产物、Runtime assembly 与 package metadata
│
├── tools/                                # Host-side 命令、服务和 UI
│   ├── bin/                              # 稳定命令入口
│   ├── bazel/                            # Bazel rules、platform、runner 和 tests
│   │   └── desktop_layout/               # Desktop layout 生成与校验 rule
│   └── <tool>/
│       ├── src-or-package/               # Tool implementation
│       ├── tests/                        # Tool tests，可选
│       └── README.md                     # Tool usage，可选
│
├── third_party/                          # Upstream integration metadata
│   ├── <upstream>/                         # 必须提交到仓库的少量 vendored source
│   └── <dependency>.BUILD.bazel            # GizOS-owned Bazel overlay
│
├── guides/                               # GizOS 项目文档
│   ├── .vitepress/                       # VitePress config
│   ├── api/                              # API Reference generator
│   ├── references/                       # locale-neutral API Reference pages
│   ├── zh/                               # 中文文档
│   │   ├── developing/                   # 开发指引
│   │   ├── using/                        # 使用说明
│   │   ├── reviewing/                    # Review 指引
│   │   ├── coding-styles/                # 代码风格约定
│   │   ├── guide.md                      # 开发指引总览
│   │   └── index.md                      # 中文首页
│   ├── en/                               # 英文入口，可为空或占位
│   ├── package.json
│   └── index.md                          # VitePress 根首页
│
└── x/                                    # 仓库内实验代码，可选
```

## 根目录文件

- `AGENTS.md` 是 Agent 使用的仓库入口，只保存索引、稳定根目录和必须遵守的工作规则。
- `.gitattributes` 和 `.gitignore` 保存 Git 行为；公开 upstream archive 的 URL、commit、解压根和 SHA-256 pin 统一由 `MODULE.bazel` 保存。
- `Makefile` 只定义公共 target、默认变量、export 和对 `scripts/` 同名脚本的调度。除 `common/` 外，每个 `scripts/<category>/<target>.py|sh` 必须只拥有对应的一个 target entry；两个以上 target 共用的 Make 实现只放在 `scripts/common/`。
- `.bazelversion`、`MODULE.bazel`、`REPO.bazel` 和根 `BUILD.bazel` 定义
  [Bazel 构建与 CI 依赖图](/zh/developing/bazel)。Bazel 是 stable host
  C/C++ package 以及 ESP-IDF/BK7258/BK3633 firmware 的入口、产物和 CI graph
  source of truth；平台 SDK 仍拥有底层原生编译语义。全部 maintained ESP 与
  BK7258 firmware entry 与三个 maintained BK3633 entry 通过受控 Bazel external rule 调用原生构建，不保留
  graph-only firmware leaf。
- VitePress 的 `package.json` 和 `package-lock.json` 属于 `guides/`，不放在仓库根目录，也不承载设备固件依赖。

不要在仓库根目录新增通用 `src/`、`include/`、`common/`、`support/` 或按 SDK 版本命名的源码目录。代码必须进入明确的 ownership root。

## `libs`

`libs/<library>/` 保存与具体 platform、chip 和 board 无关的仓库级可复用库。`libs/pal/providers/<platform-or-sdk>/` 保存同样由 Bazel graph 编译、但只对某个平台成立的实现。两类 library 都不能拥有 board wiring、具体固件入口、firmware archive registration 或 project policy；把 Bazel archive 接入原生 SDK build system 的 composition target 属于最终 firmware entry。

每个 library 的标准结构是：

```text
libs/<library>/
├── include/
├── src/
├── tests/
└── BUILD.bazel
```

每个 library 都有独立的 `BUILD.bazel`。`BUILD.bazel` 通过显式 `srcs`、`hdrs`、`data` 和 `deps` 定义 library target 与 tests，测试目录统一使用 `tests/`。CI 直接分析所选平台的完整 compatible graph，不通过 tag 维护第二份 library 或 artifact inventory。平台差异只需要 toolchain 或 compatibility 即可表达时，不复制 source tree；只有接入 API、构建系统或 OS service 不同时，才在 `libs/pal/providers/` 下建立具名平台边界。

Library 集成的 upstream dependency 需要补充 CPU/ABI implementation 时，修改 upstream selection 的 patch 与新增 backend source 统一归 `third_party/<dependency>_patch/`；vendor repository 将它们应用到固定 upstream checkout，`libs/<library>` 仍只暴露一个 semantic target。Backend 可以使用目标 compiler/ABI，但不能取得 SDK lifecycle、board wiring 或 firmware policy；upstream checkout 本身保持未修改。

PIXA Games 使用 library family 结构：

```text
projects/pixa_games/libs/
├── dinorun/
├── dinodive/
├── dinobounce/
└── dinotetris/
```

`projects/pixa_games/libs` 只作为 family container，不能直接放 `include/`、`src/` 或 shared implementation。每个 `projects/pixa_games/libs/<game>/` 都是独立 reusable library，遵守普通 library 的 `include/`、`src/`、`tests/` 和 `BUILD.bazel` 边界。Host 只向需要 player 的 game 传入 shared Player PIXA；game-owned legacy UI 贴图和样式栅格化结果生成到 `src/` 的 ARGB4444 代码数据，HUD 等运行时几何由 PixelRoot 绘制，不新增聚合的 `environment.pixa`。Game library 只关心自身 required button role、gameplay、PixelRoot scene、Player PIXA clip contract 和 result，不依赖引用它的 project。

H106、Desktop 或其他 project 只引用需要的 game library，并在 host main loop 中把自己的 Runtime `component_id` 映射成 game button role。Host component 到 board `periph_id` 的 mapping 仍由对应 launcher 提供。具体 contract 见 [PIXA Games](/apps/pixa_games)。

`libs/pal/include/` 是特殊边界，只保存 PAL contract：`h2_pal.h` 是根级全量 aggregate header，其他 public header 按 `h2/pal/core`、`os`、`net`、`application` 和 `hal` 分类，canonical unsupported accessor 位于 `h2/pal/h2_pal_unsupported.h`。这些目录仍属于同一个 `libs/pal` Bazel package 和 header-only `//libs/pal:pal` target，不引入子 package、分层 contract target 或新的 provider/runtime boundary。`libs/pal/src/unsupported/` 只保存所有平台共用的 canonical unsupported API object，并按 capability 保持一个 translation unit；直接使用 accessor 的 consumer 依赖 `//libs/pal:unsupported`，由静态链接器按 object 抽取。PAL 仍然必须遵守相同的 `tests/` 约定。真实 PAL backend、dummy backend、target-specific unavailable adapter 和 test fake 属于 `libs/pal/providers`、原生 SDK `components`、BSP 或 test adapter，不能放进 PAL contract package。

`libs/drivers/` 可以继续按 modem、motion、NFC 等 portable driver family 拆分，但 driver 不能拥有具体 board 的 pin、bus instance 或 wiring。

## `native_component_src`

`native_component_src/<sdk-family>/` 保存由原生 SDK build system 编译的 target 组件源码。普通 Bazel `cc_library` 平台实现属于 `libs/pal/providers/`。拥有源码的语义组件必须用 `firmware_native_component` 显式声明自己的 component name、可选 AP/CP execution unit、direct `srcs`、`hdrs`、include roots、`data` 和 `deps`。每个最终 firmware entry 必须用唯一一个 `firmware_lib_component` 列出该 image 的全部 Bazel library；runner 把它们的 archive closure 作为单个 `h2_firmware_lib` 原生组件和一个 rescan link group 交给 SDK。Library 和普通 native component 只声明自己的源码依赖，不能取得某个 image 的 library composition ownership。没有自己的 SDK dependency、registration lifecycle 或 native source 时，不能只为 `add_prebuilt_library()` 创建独立 `native_component_src` 目录。Firmware rule 只遍历显式 `graph`，runner 再生成 invocation-local CMake/Make manifest；原生 build file 不能扫描 checkout、恢复 recursive glob 或维护第二份 first-party source/component inventory。Component 可以：

- 封装芯片 SDK、RTOS 或 platform service。
- 提供同一 target 上多个 board 可以复用的 driver glue。

Component 不能拥有物理 board GPIO、board defaults、具体 app 业务或某个 firmware image 的启动策略。

Multi-core 或 multi-MCU target 按运行单元继续拆分：

```text
native_component_src/<chip>/ap/
native_component_src/<chip>/cp/
```

ESP-IDF 6.x components 统一放在 `native_component_src/esp-idf6.x/`。ESP32-S3、ESP32-P4 和 ESP32-C5 的差异通过 `IDF_TARGET`、target-specific source selection 或 component config 表达，不按 chip 复制 component root。Library 已经由 Bazel 编译为目标平台 archive 时，由具体 firmware entry 的唯一 `firmware_lib_component` 直接选择；只有还需要独立 SDK dependency、Kconfig、registration lifecycle 或 native source 时才建立 `firmware_native_component`。没有该 native contract 的 archive 不能创建仅转发 include 和 `.a` 的目录。`libs/pal/providers/` 只拥有由 Bazel C/C++ toolchain 直接编译的平台实现，不拥有原生 SDK registration adapter。

Linux userspace 通用 PAL backend 归 `libs/pal/providers/linux/`；Darwin host backend 归 `libs/pal/providers/darwin/`；Allwinner CedarX 等 vendor SDK family integration 归 `libs/pal/providers/allwinner-linux/`。通用 OS component 不能依赖具体 board、App、Desktop target family 或 project launcher。Linux 和 Darwin 需要共用 POSIX 实现时，可以依赖 `libs/pal/providers/posix/` 的 private source-sharing target；该 root 不提供公共 provider API，也不能被 App 或 Desktop component 直接依赖。

跨 Linux/macOS Desktop 共用的 adapter、simulator package 和 Desktop-specific tests 归 `libs/pal/providers/desktop/`，不放入 OS backend 或设备 board BSP。纯 upstream SDL3、PortAudio 与 FFmpeg 由 `third_party/*.BUILD.bazel` 暴露稳定 vendor label；Desktop PAL 或 adapter 直接消费这些 label，不读取 host prefix。SDL3 只属于 Desktop host，不进入 T113 或其他嵌入式 board graph。Desktop root 不拥有 Netif、SystemEvent、Host Serial、CoreBluetooth 等 OS capability，也不能成为 Linux/Darwin target 复用这些能力的前置依赖。

`libs/pal/providers/desktop/app_support:app_support` 是 Linux/macOS Desktop Runtime composition owner：它按 Bazel target platform 选择 Linux 或 Darwin provider，再把 PAL API object 注入跨平台 Desktop integration。Windows 原生 OS capability 归 `libs/pal/providers/windows/pal_core`，不经过 Desktop-named accessor 或 forwarding facade；Windows Desktop 产品组装仍由具体 project owner 完成。所有 OS provider 都不能依赖另一个 OS provider 或 Desktop 实现。

iOS、Android 和 Browser/WebAssembly 的仓库级 PAL backend 分别归 `libs/pal/providers/ios/`、`libs/pal/providers/android/` 和 `libs/pal/providers/web/`。它们提供 reusable platform capability，不选择 App、不组装完整 Runtime，也不能依赖具体 project 的 App、adapter 或 artifact entry 类型。

## `boards`

`boards/<board>/<chip-or-target>/` 保存物理 board BSP：

- GPIO、pin mux、bus、address 和 channel。
- 板上实际外设及其 `periph_id`。
- Board-only driver instance 和 hardware defaults。
- BSP public API 和 runtime config provider。
- Board-specific SDK defaults 或 partition 输入。
- Board firmware profile，放在 `layouts/<profile>/`，包含该启动与升级模型共用的 partition、layout-specific SDK defaults、link 和 recovery 输入。

同一个物理 board 包含多个 chip 或 core 时，它们仍放在同一个 `<board>` 根目录下，再按 `<chip-or-target>` 拆分。例如 BK7258 的 AP/CP 或同时包含 ESP32-P4、ESP32-C5 的开发板。

只有负责运行 Runtime 的 BSP 才需要提供 runtime config；辅助 chip/core 只暴露自身职责需要的 public API。

## `projects`

`projects/` 同时保存 portable project 和 project-owned artifact entry。Portable project 拥有 App、assets/data、project-local library 和 project policy；artifact entry 负责把这些 App 组装成某个平台可执行、可安装或可加载的产物，但不取得 portable App 源码 ownership。仓库中的 portable project 支持独立 App 与 project group 两种形态。

### Artifact-rule roots

最终产物直接归拥有 App 的 project，并统一放在 `targets/` 下，以 Bazel rule 名作为产物类型目录：

```text
projects/<project>/targets/<artifact-rule>/<app>[/<variant>]/
```

当前稳定 rule roots 包括 `cc_binary`、`macos_application`、`ios_application`、`android_binary`、`h2loader_tar_zlib`、`bk3633_firmware` 和 `jieli_firmware`。`jieli_firmware` entry 消费 firmwares-devenv 通过 `JIELI_*` locator 提供的杰理 SDK checkout 与解包工具链，SDK 不进入本仓 submodule。目录名描述最终交付物的 rule 类型，App 名描述业务入口；只有同一 App 的原生构建必须按 board 隔离、或多个 package 必须保留相同本地 target 名时，才增加 `<variant>`。H2Loader package root 内部可以用 `esp_idf_firmware` 或 `bk7258_firmware` 生成平台原生固件，但平台 rule 不是该目录的最终交付物。

`targets/cc_binary/<app>` 保存普通 host 或 Linux executable 的薄 main、Runtime assembly、layout 和显式 runtime data。Desktop 可复用支持仍归 `libs/pal/providers/desktop/`，project 私有、由 Bazel 编译的 Desktop glue 归 `projects/<project>/libs/desktop*/`。

### Mobile Entry

Mobile 产物同样归真实 App owner。例如 Tap Reset 使用：

```text
projects/example/libs/mobile/tap-reset/
projects/example/targets/ios_application/tap-reset/
projects/example/targets/android_binary/tap-reset/
libs/pal/providers/ios/pal_core/
libs/pal/providers/android/pal_core/
```

App-specific Mobile adapter 与 contract conversion 归 project 的具名 `libs/mobile/<app>/` Bazel library，继续传递同一个 `h2_runtime_t *`，且不能 include UIKit、Android JNI 或 launcher-private type。Portable App 源码仍只位于 `apps/<app>/app/`。iOS 与 Android rule root 分别保存 lifecycle、Runtime assembly、package identity、metadata 和最终 `.app`/APK 入口。Display、Memory、Time、Queue 等跨 project PAL backend 仍归 `libs/pal/providers/ios` 或 `libs/pal/providers/android`。

### Web Entry

Web 入口也归 portable App owner。最终交付物使用实际 Bazel packaging rule 命名的 root：

```text
projects/example/libs/web/tap-reset/
projects/example/targets/pkg_tar/tap-reset/
libs/pal/providers/web/pal_core/
```

Project-local Web component 保存 presentation、required capability 和 portable App contract conversion；`pkg_tar/<app>` 保存 Emscripten lifecycle、Runtime assembly、HTML shell 和最终 serve-ready Web archive。内部编译步骤使用 `wasm_cc_binary`，最终 rule 使用 `pkg_tar`，归档根目录直接提供 `index.html` 及其 JS/WASM 依赖。Web wrapper 不能依赖 Mobile contract。跨 project 的 Canvas、pointer、Memory、Time 与 Queue backend 属于 `libs/pal/providers/web/pal_core`。

App 或 library 的 Bazel target 只在 source、defines、toolchain compatibility 或 dependency graph 存在实际差异时拆成 `_embed`、`_desktop`、`_mobile`、`_web` variant；没有差异时保留无后缀 target。Variant 按运行环境命名，不能按具体 App 复制公共 library。

### Embedded Linux Entry

普通 Embedded Linux executable 使用 App owner 的 `cc_binary/<app>/<board>`：

```text
projects/example/targets/cc_binary/<app>/kickpi_k4b/
├── BUILD.bazel
├── main/
└── package/
```

该 package 只拥有具体 board 上的 Runtime assembly、executable entry、build config、service unit 和 package metadata。Portable App 仍由同一 project 的 `app/` 或 `apps/<app>/app/` 持有；可复用 Linux PAL backend 属于顶层 `libs/pal/providers/linux`，物理接线与默认值属于 `boards/`。

Launcher 使用 Bazel 生成 executable，并把 service metadata、resource 与需要的 target runtime library 声明为显式 data。设备发现、设备选择、应用烧录、运行、状态查询和恢复由开发环境提供的标准 ADB 完成，具体流程见 [Embedded Linux 使用说明](/zh/using/embed_linux/)。设备必须已经运行可用的 Linux userspace 与 `adbd`；制作或安装 OS/rootfs 不属于 App launcher 的职责。

### Examples

`projects/example/` 是 target-independent runnable Example project group。每个 Example 位于 `projects/example/apps/<app>/app/`，通过可观察场景展示 Runtime、PAL、portable library 与 launcher 的组合方式，可以有意覆盖多个相关能力，也可以同时被 Desktop、iOS、Android、Web 或设备 launcher 消费。

Example-owned 的最终产物按 rule 放在 `projects/example/targets/<artifact-rule>/<app>[/<board>]`。该 entry 只拥有目标 Runtime、SDK、Board 组装、build entry 和产物路径，可以消费对应 `boards/`、`libs/pal/providers/` 与 `native_component_src/`；它不拥有产品 App 或产品 policy。

Example 不能 include target SDK、launcher private type 或 H2Loader product policy。归属只取决于 public Runtime/PAL contract 和行为是否与产品 lifecycle ownership 解耦，与当前 consumer 数量无关。具体 board 与 platform launcher 决定自己提供哪些 Example；portable App 不按平台身份分支，也不把缺少 required capability 转换成运行时 `SKIP`。只有行为本身用于验证某个 board、image、transport 或 H2Loader confirmation、rollback、update lifecycle 的 fixture 才继续留在对应 owner project。

### E2E 测试 App

`projects/e2e/` 是 reusable cross-target headless test App project group。每个 portable registry 位于 `projects/e2e/apps/<test-app>/app/`，持有机器可验证的 case、deadline、progress、non-fail-fast aggregation、result schema 与 App-owned cleanup；library-local unit、fake、parser 和 protocol test 仍留在对应 library owner。

Portable E2E App 只依赖 Runtime/PAL 和被测 target-independent library，不能依赖 Desktop、OS、Board、SDK、H2Loader、process environment、host filesystem path 或具体 backend。Desktop 与 firmware launcher 持有 Runtime/provider assembly、endpoint/fixture 注入、platform lifecycle 与结果输出。Provider 名属于 launcher target，不通过复制 portable registry 表达。

H2Loader-managed E2E image 位于 `projects/e2e/targets/h2loader_tar_zlib/<image>/<board>/`。`h2loader_tar_zlib` 表示安装产物类型，不改变 E2E App ownership。没有 H2Loader 的 standalone diagnostic image 位于 `projects/e2e/targets/<firmware-rule>/<image>/<board>/`。比如 BK3633 Libco Smoke 位于 `projects/e2e/targets/bk3633_firmware/libco-smoke/tapdoki_v2_0/`；它使用 TapDoki v2.0 Board 与 BK3633 component，但不属于 TapDoki production project。

### 独立 App

```text
projects/<app>/
├── app/
│   ├── include/
│   └── src/
├── assets/                               # 固件实际消费的资源，可选
├── data/                                 # App package data，可选
└── ia/                                   # App information architecture 和产品设计输入，可选
```

App 必须提供稳定 public API。阻塞式 entry 必须接收 `h2_runtime_t *`，也可以接收该 app 自己定义的 stable public config 或其他 app-level 参数；不能接收 BSP object、board-private type、芯片 SDK object、原始 PAL backend 或 launcher private type。App 通过 Runtime 和跨平台 `libs` 使用能力。

独立 app 目录只定义 portable app ownership，不保证存在 Desktop executable 或其他 target launcher。

`ia/` 属于具体 app 的可选 project-local 产品设计输入，可以保存信息架构、screen specification、user story 和配套图示。产品把这些合同迁移到 `guides/` 后应删除重复 IA，避免同时维护两份 source of truth。固件编译或打包直接消费的图片、字体和其他资源仍属于对应 project 的 `assets/`，不与 IA 设计源混放。

### Project Group

一组共同维护的 portable app 使用以下结构。Project group 的最终产物统一放入 `targets/`，原生 SDK source 统一放入 `native_component_src/`：

```text
projects/<group>/
├── libs/
│   ├── cmake/                            # Reusable library CMake build API，可选
│   └── <library>/                        # Project-owned portable library
├── native_component_src/<platform-or-target>/ # Group-local native SDK component source
├── tools/<tool>/                        # Group-owned host/build tool 与配置
├── apps/<app>/                           # Portable apps
└── targets/<artifact-rule>/<app>[/<variant>]/ # Final artifact entries
```

`libs/` 只是 project-local library 的容器，不能直接在 `libs/` 下放 `include/` 和 `src/`。每个 library 必须使用 `projects/<group>/libs/<library>/` 具名，并按照普通 library 的 public API、portable implementation、tests 和 Bazel package 边界组织。`projects/<group>/libs/cmake/` 是具名的可复用 build library，其 public API 是供多个 target build system 调用的 CMake function；它只能描述该 group library 的 source inventory、include 和 portable compile contract，不能取得 board wiring、image policy 或 target component ownership。

`native_component_src/` 保存只服务当前 project group、必须由原生 SDK build system 编译的 component source，例如 project protocol 的 SDK adapter、project runtime glue 或 image integration。它按 platform、SDK family 或 target 继续分层，不保存 Bazel library、物理 board wiring、最终 artifact entry 或 app 业务代码。由 Bazel `cc_library` 编译的 project-owned reusable code 必须位于具名 `libs/<library>/`；最终 executable、bundle、package 或 firmware entry 必须位于 `targets/<artifact-rule>/`。Project root 不再使用 `components/` 作为 ownership root。

Project-owned library 可以通过稳定 public API 被其他 project 依赖；依赖方向必须从 consumer 指向 provider，provider 不能反向依赖具体 consumer，也不能形成 project dependency cycle。只有没有明确 project owner 的仓库级 portable capability 才放在顶层 `libs/`。Project-local native component source 仍只服务当前 group；可被多个 project 使用的 Bazel 平台实现应提升到 `libs/pal/providers/`，由原生 SDK 编译的共享源码才提升到顶层 `native_component_src/`。

H106 使用 project group 形态，并直接拥有自己的最终产物入口：

```text
projects/h106/
├── apps/
│   ├── main/app/                         # 正式 H106 portable App
│   └── mfg/app/                          # H106 portable MFG App
├── assets/                               # Main/MFG 共用制作源与发布资源
├── tools/
│   ├── assets/                         # H106 专属资源发布与一致性校验
│   └── i18n/                           # H106 翻译配置、生成驱动与 UI literal 校验
└── ia/                                   # H106 产品与流程设计输入

projects/h106/targets/cc_binary/main/              # Shared main；Bazel targets 选择 Tiga/Zero defines
projects/h106/libs/desktop/main/     # 两个 H106 entry 共用的 Desktop target component
projects/h106/targets/h2loader_tar_zlib/main/<board>/
                                             # Main App 的 H2Loader package image
projects/h2loader/targets/h2loader_tar_zlib/loader/<board>/
                                             # 固定 Loader image；需要时先运行 MFG App
```

`apps/main` 与 `apps/mfg` 是两个独立 portable App contract，不能共享 component id enum、private state 或 entry config。H2Loader launcher 可以连接 H106 App，但不会因此取得其业务源码 ownership；MFG App 编入固定 Loader image 时也不创建独立可安装 App image。

`projects/h106/tools/` 只保存 H106 project-owned host tool。资源 publisher 可读取 H106 的 raw 和 production asset contract，但不能成为其他 project 的通用资源转换入口。

Final artifact entry 同时知道具体 App 与具体 target，负责 SDK/build config、BSP wiring、component-periph mapping、Runtime 生命周期和 App entry 调用。Entry 必须保持薄，可复用逻辑返回 App、project-local library、BSP、component 或仓库级 library。

Firmware final artifact entry 还负责选择 image-specific SDK role、connection/activity capacity、prebuilt Stack/BIM/OAD 输入和 configuration header。物理 Board 只提供 wiring、外设、Flash/RAM 约束和可选 linker layout，不能把一个 project 的 image policy 作为所有 consumer 的默认配置；同一 Board 的不同 project 必须能够显式选择不同配置。

Showcase 是跨产品型号复用的独立 App，不属于 H106 project group：

```text
projects/showcase/
├── app/                                  # Portable Showcase App
├── assets/                               # 展架角色与 fallback 资源
├── data/                                 # 可选的 App package data
└── ia/                                   # 展架产品与页面设计输入

projects/showcase/targets/cc_binary/showcase/            # Showcase Desktop entry
```

`projects/showcase/apps/showcase/` 拥有展架业务状态机、管理 command 和 portable UI；各产品型号通过自己的 board、target component 和 launcher 提供按键、SD 卡、显示、音频、网络与开机服务。Showcase 不能依赖 H106 private state、资源目录、component id 或 launcher wiring。

### PIXA Games Projects

PIXA Games 使用 project group 保存 portable game App shell 及其 project-level 输入：

```text
projects/pixa_games/
├── libs/
│   ├── cmake/
│   │   └── pixa_game.cmake               # Reusable firmware build API
│   └── <game>/                           # Reusable game public contract 与 implementation
├── apps/<game>/
│   ├── app/                              # Portable Runtime/game host App
│   ├── assets/                           # 上游 attribution 与制作输入，可选
│   ├── data/                             # App package data，可选
│   └── BUILD.bazel
```

`projects/pixa_games/apps/<game>/app/` 负责 Runtime lifecycle、game input role mapping、Display/Audio/Filesystem 接线和阻塞式 App entry。Reusable gameplay、scene 和 game public contract 属于 `projects/pixa_games/libs/<game>/`，可以被 H106、Desktop 和其他 consumer project 直接引用。Firmware entry 通过 `projects/pixa_games/libs/cmake/pixa_game.cmake` 把 portable game sources 接入已有 image component，不为每个 game 创建 ESP-IDF 或 BK component wrapper。H2Loader package entry 和 Desktop executable entry分别位于 `projects/pixa_games/targets/h2loader_tar_zlib/<game>/<board>/` 与 `projects/pixa_games/targets/cc_binary/<game>/`；它们消费 portable game App，不能复制 App implementation 或取得 gameplay ownership。

### H2Loader Project Group

H2Loader 的完整目录结构是：

```text
projects/h2loader/
├── libs/
│   ├── h2loader/
│   │   ├── include/                      # Shared H2Loader contract
│   │   └── src/                          # Package、image、boot portable core
│   ├── web/                              # Internal reusable H2Loader JS/runtime/WASM SDK
│   └── cmake/                            # Shared H2Loader package helper
├── native_component_src/                 # H2Loader-only native SDK component source
│   ├── esp-idf6.x/
│   │   ├── h2_h2loader_runtime/          # ESP Loader/App base runtime glue
│   │   └── h2_h2loader_ble/              # Entry-selected BLE command service
│   └── bk7258/
│       └── ap/
│           └── h2loader_bk/
├── apps/
│   ├── loader/                           # H2Loader 固件 app
│   │   └── app/
│   ├── batch-loader/                     # Batch Loader React/shadcn Web App
│       └── app/
│   └── cli/                              # Portable native CLI App
│       └── app/
├── tools/
│   └── bazel/                            # H2Loader package、recovery 与 artifact build helper
└── targets/
    ├── cc_binary/cli/                    # Native CLI process 与 host PAL assembly
    ├── pkg_tar/batch-loader/              # Batch Loader HTML/JS/WASM Web archive
    └── h2loader_tar_zlib/loader/<board>/  # Loader package；内部选择 ESP-IDF 或 BK7258 firmware
```

设备端 Loader App、共用 library、H2Loader 专用 target component、Loader image entry 和 H2Loader package tooling 归 `projects/h2loader/`。`projects/h2loader/tools/bazel/` 拥有 `h2loader_tar_zlib` artifact rule、按 `target`/`board` 选择 layout 的 firmware wrapper，以及共用 partition、recovery 和 variant build input；全仓通用 ESP-IDF、BK firmware rule 仍归顶层 `tools/bazel/`。可复用 Example 归 `projects/example/apps/`，跨目标测试 App 归 `projects/e2e/apps/`，PIXA game App 归 `projects/pixa_games/apps/`；它们的 H2Loader-managed package 同样位于各自 project 的 `targets/h2loader_tar_zlib/`。被 H2Loader 安装不会改变源码或 artifact ownership。Batch Loader React/shadcn UI 归 `projects/h2loader/apps/batch-loader/app/`，内部可复用 Web SDK 与唯一 archive 分别归 `libs/web/` 与 `targets/pkg_tar/batch-loader/`。Native CLI App 与 executable target 分别归 `projects/h2loader/apps/cli/app/` 和 `projects/h2loader/targets/cc_binary/cli/`；portable Host Core 归 `libs/h2loader_host/`。具体职责见 [H2Loader 产品文档](/apps/h2loader/)。

## `tools`

`tools/` 保存只在开发机运行的命令、生成器和设备操作工具。

主要形态包括：

```text
tools/
├── bazel/                                # Bazel rules、platform 与 build-only helpers
├── lvgl/
│   ├── conv_i18n_yaml/                   # 通用 YAML 到 LVGL static translation C/H 转换器
│   └── conv_png/                         # 通用 RGBA PNG 到 LVGL ARGB8888 C descriptor 转换器
├── gizclaw-ping-speed/
├── mklittlefs/
└── webrtc-test-server/
```

Host tool 可以发现、构建和运行 project artifact entry，但不能成为设备端 portable app 的源码 ownership root。

## `third_party`

`third_party/` 保存 GizOS-owned BUILD overlay、source overlay、兼容 patch，以及确实需要提交的少量 vendored source；公开 upstream source 不以 Git submodule 保存。每个公开 dependency 的 immutable commit archive URL、SHA-256 和 extracted root 由 `MODULE.bazel` 唯一固定，`tools/bazel/vendor/repositories.bzl` 在 external repository 中先下载并校验 archive，再装配 repository-owned metadata。Overlay 不能修改下载缓存，也不能反向引用仓库内的 library、component、project、board 或 tool target。Overlay 默认只提供 source/header/file group；需要 PAL、GizOS API 或 platform dependency 的编译规则属于对应 `libs/` 或 component owner。仓库明确直接使用的纯 upstream contract 可以在 overlay 中直接编译；仅用于补齐 upstream portable build 的 config header 放在 `third_party/<dependency>_patch/`，并通过 repository rule 的 `overlay_files` 在 analysis 前 materialize 到 external repository。该 config 不能 include GizOS header、改变 public API 或取得 platform lifecycle ownership。CMake/configure upstream 可以使用 `rules_foreign_cc`，但 action input 必须只来自已校验 archive、显式 overlay、Bazel toolchain 和声明的 upstream dependency。此类 target 对 consumer 暴露不带 host 后缀的稳定 label，平台选择只存在于 overlay 内部；`libs` 和 `native_component_src` 可以直接依赖它，不能为了转发同一个 upstream target 再创建 `native_component_src/<platform>/pkgs` wrapper。通用 repository rule、必要的 upstream configure adapter 和 graph tool 仍属于 `tools/bazel/`。

GizOS 自己的跨平台 API 和 integration 应放在 `libs`，由原生 SDK 编译的 target adapter source 放在 `native_component_src`。App、BSP 和 launcher 不应直接把 third-party contract 当作 GizOS public API。

## `guides`

`guides/` 是新的 GizOS 项目文档入口：

```text
guides/
├── .vitepress/
│   └── config.mts
├── api/                                  # Doxygen API 生成器
├── en/                                   # English documents
├── references/                           # API Reference pages
├── zh/
│   ├── developing/
│   ├── reviewing/
│   ├── coding-styles/
│   └── guide.md
├── index.md
├── package.json
└── package-lock.json
```

`.vitepress/cache/` 和 `.vitepress/dist/` 是生成产物，不属于文档源码。

## `x`

`x/` 用于仓库内实验代码，不属于稳定 contract。稳定 app、library、component 或 BSP 不能反向依赖实验目录。

## 生成产物

以下路径是依赖或构建产物，不是源码 ownership root：

```text
node_modules/
build/
bazel-*
guides/.vitepress/cache/
guides/.vitepress/dist/
<sdk-project>/build/
```

生成产物不能作为 include、package、asset 或运行时输入的稳定来源，也不能提交成新的源码目录。需要提交的 generated contract 或 fixture 必须由对应 library、project 或 tool 明确拥有，并提供可重复生成和验证的方法。
