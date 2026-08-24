# 项目结构

本文只定义 H2Loader 源码目录及其 ownership。固件运行时角色和分区约束见 [固件结构分区与类型](./firmware_types)，安装和启动状态见 [更新、启动与回退](./update/)。

## 项目布局

```text
projects/h2loader/
├── apps/
│   ├── loader/app/                       # Portable Loader App
│   ├── batch-loader/app/                  # H2Loader Batch Loader React/shadcn Web App
│   └── cli/app/                           # Portable native CLI App
├── libs/
│   ├── h2loader/                         # H2Loader 公共库
│   ├── web/                               # Internal reusable H2Loader JS/runtime/WASM SDK
│   └── cmake/                            # 可复用 image build helper
├── native_component_src/
│   └── <target>/                         # H2Loader 专用原生 SDK component source
├── tools/bazel/
│   ├── h2loader_firmware.bzl             # 按 target/board 注入 H2Loader firmware layout
│   └── h2loader_tar_zlib.bzl             # H2Loader package artifact rule
└── targets/
    ├── cc_binary/cli/                     # macOS/Linux/Windows native CLI process 与 PAL 组装
    ├── pkg_tar/batch-loader/               # Batch Loader HTML/JS/WASM Web archive
    └── h2loader_tar_zlib/loader/<board>/  # Loader package 与内部平台 firmware

projects/example/apps/<app>/app/          # 共享 target-independent Example
projects/e2e/apps/<test-app>/app/         # 共享 target-independent E2E 测试 App
projects/pixa_games/apps/<game>/app/      # Portable PIXA game App
projects/<group>/apps/<app>/app/           # 其它产品拥有的 portable App
projects/<owner>/targets/h2loader_tar_zlib/<image>/<board>/ # Owner 的 H2Loader-managed package
boards/<board>/<target>/                   # 物理 board BSP、defaults 与 firmware layouts
native_component_src/<target>/             # 跨产品原生 SDK component source
libs/h2loader_host/                        # Portable Host Core
projects/h2loader/apps/batch-loader/app/   # Batch Loader Host UI
projects/h2loader/apps/cli/app/            # Portable CLI command 与 output policy
projects/h2loader/targets/cc_binary/cli/   # Native CLI executable target
```

## App 入口

目录位置：`projects/h2loader/apps/`

`apps/loader/app/` 保存 portable Loader App。它组织设备端 command、安装和启动流程，但不包含 board、partition label、SDK startup 或 Host UI。

`apps/batch-loader/app/` 保存 React JavaScript/JSX DOM App、shadcn components、本地 package dialog、device table、localStorage metadata 和最多四路的 batch controller。内部 Web SDK 位于 `libs/web/`，通过 public Promise API 包装 Host Core 与 Web PAL；App 不使用 raw Emscripten ABI。最终 serve-ready archive 位于 `targets/pkg_tar/batch-loader/`。

`apps/cli/app/` 保存 portable C11 CLI App。它拥有参数校验、command dispatch、stable output 和 command-scoped policy，只消费 Runtime、现有 PAL contract、`libs/command/` 与 `libs/h2loader_host/`。`targets/cc_binary/cli/` 只拥有 argv/stdio、signal cancellation 和 macOS/Linux/Windows PAL provider 组装，不定义 project-private OS capability。

`projects/example/apps/<app>/app/` 保存 reusable Example，`projects/e2e/apps/<test-app>/app/` 保存跨目标 headless test App，`projects/pixa_games/apps/<game>/app/` 保存 portable game App，H106 等产品 App 保留在自己的 project group。被 H2Loader 安装不改变 App 或最终 artifact 的 project ownership。

各 App owner 在自己的 `targets/h2loader_tar_zlib/<image>/<board>/` 中提供 Runtime、App command/confirmation、package identity、board wiring 和最终 package。它们可以依赖 H2Loader library、native component 与 packaging rule，但这些 adapter dependency 不把 artifact ownership 转移给 H2Loader。

## H2Loader 共用代码

目录位置：`projects/h2loader/libs/h2loader/`

这里是 H2Loader 的 project-local library，保存 Loader firmware 与 H2Loader-managed App firmware 共用的稳定 API 和跨平台实现，包括：

- Package、image identity 与安装状态。
- App confirm、restart、rollback 与 return-to-loader。
- Loader/App boot intent。
- Device command handler 与结构化 status/stats。

共用代码不包含 Runtime 初始化、BSP wiring、SDK object、具体 partition label、Host transport backend 或 App 业务。

[API Reference](/references/h2loader)

## H2Loader 专用组件

目录位置：`projects/h2loader/native_component_src/`

这里保存只服务 H2Loader 的 target component，例如 ESP H2Loader command runtime 或 BK AP adapter。

- 可以依赖 H2Loader 共用代码和顶层 target component。
- 不能拥有物理 board GPIO、partition layout 或某个 App 的业务。
- 能被 H2Loader 以外产品复用且由原生 SDK 编译的实现必须放到顶层 `native_component_src/<target>/`；Bazel 直接编译的平台实现放到 `libs/pal/providers/`。

ESP safe-call、BK AP/CP worker 等 SDK 实现细节属于对应 target component 文档，不进入 H2Loader 产品通用文档。

## 固件构建入口

目录位置：`projects/<owner>/targets/h2loader_tar_zlib/`

每个 `projects/<owner>/targets/h2loader_tar_zlib/<image>/<board>/` 都是一个可独立构建的 H2Loader package 入口。`<owner>` 是真实 App owner；`h2loader_tar_zlib` 只表示安装产物类型。Entry 同时知道 portable App 与物理 board，因此负责：

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
`target` 与 `board` 从 `boards/<board>/<target>/layouts/` 注入 partition、layout
defaults 和 recovery 配置。通过 Bzlmod 消费 GizOS 的下游仓库可以保留自己的私有
Board，并通过 `layout_files` 显式传入该仓库拥有的完整 layout labels；私有 Board
不得加入 GizOS 的内建 Board registry，也不得复制 H2Loader wrapper。最终 SDK 配置
按 board defaults 和 layout defaults 的顺序合并。CMake 只消费 runner 生成的 component/source
manifest，不再维护第二份 first-party component path 或 source inventory。

一个 portable App 只有一份实现；每个支持它的 board 仍需要单独 variant，因为 BSP、partition、component mapping 与 SDK build root 不同。

ESP-IDF entry 的 CMake project 仍拥有 component discovery、Kconfig、linker、partition 和 bootloader 语义。内部 `:firmware` 使用 `h2loader_esp_idf_firmware` 按 `target` 与 `board` 选择 native layout，再委托通用 `esp_idf_firmware` 在 action-owned tree 内调用原生 `idf.py build`。它只暴露结构化 ELF、map、app、bootloader、partition table、由同一次构建的 flash arguments 生成并从 `0x0` 烧录的 `combined_factory.bin`、flash metadata 和完整 flash file set；Loader recovery 由外层 package rule 生成，并把 combined image 仅作为 native factory/recovery artifact 打包。公开交付从 `bazel build --config=<esp32s3|esp32p4> //projects/<owner>/targets/h2loader_tar_zlib/<image>/<board>:package` 进入；`:package` 使用平台无关的 `h2loader_tar_zlib` 消费 `:firmware`，生成唯一 H2Loader package 和 release metadata。`combined_factory.bin` 不取得 managed package identity，也不参与 H2Loader runtime update。调用方不能把 action 内部的 native command 或临时 build tree 当成第二个公开入口。

BK7258 entry 的 CMake project 继续拥有 AP/CP 与 linker 语义。每块
H2Loader-managed BK7258 Board 在自己的 `boards/<board>/bk7258/` 中拥有 board
defaults，并在 `layouts/<layout>/` 中拥有 OTA flash geometry、SMP build config、
recovery config、GPIO、RAM region 和 AP/CP layout defaults；同一 Board 的 Loader 与
App 都由 `h2loader_bk7258_firmware` 注入对应 layout，target 目录不保留 SDK config。
通过 Bzlmod 使用私有 Board 时，consumer 必须在 `layout_files` 中显式提供 AP/CP
config、GPIO、RAM region 和 project-support labels。支持跨 Board OTA 兼容的 Board
必须保持相同 flash geometry，并由 graph test 比较其内容。`ram_regions.csv` 是 image
linker RAM profile 而不是 OTA partition layout。通用 `bk7258_firmware` 只暴露标准
native outputs；Loader recovery bundle 由外层 package rule 生成。

## 物理 Board 支持

目录位置：`boards/<board>/<target>/`

这里描述物理 board 的 GPIO、bus、peripheral、memory、BSP wiring 和 `layouts/<profile>/`。Board defaults 表达所有 firmware 共用的硬件条件，layout defaults 表达 `standard`、`h2loader` 等启动与升级模型共用的 partition、SDK config、link 和 recovery 输入；并非所有 image 都使用但能在同一 Board 上复用的 SDK、GPIO 或 memory 差异使用 layout 下可组合的具名 profile。最终 artifact entry 只选择 profile，不拥有 SDK config。Board 不知道某个 App 的业务状态，也不决定 package policy。

## 平台共用组件

目录位置：`native_component_src/<target>/`

这里保存同一 target family 可跨产品复用的 SDK adapter。H2Loader 专用 glue 留在 `projects/h2loader/native_component_src/`；物理板差异留在 `boards/`。

## Host 工具

目录位置：`projects/h2loader/apps/cli/app/` 和 `projects/h2loader/targets/cc_binary/cli/`

H2Loader Batch Loader Web App 位于 `apps/batch-loader/app/`，可复用 JS/runtime/WASM SDK 位于 `libs/web/`，唯一浏览器 archive 位于 `targets/pkg_tar/batch-loader/`。Batch Loader 与 native CLI 都通过 `libs/h2loader_host/` 取得 authoritative identity、typed command、package contract 与 lifecycle verification；两者不互相链接。CLI 的 App source、host target 和 tests 都由 `projects/h2loader/` 拥有，不在 `tools/` 维护第二套 protocol、package writer 或 Python runtime。Host 代码不属于 firmware，也不通过设备 Runtime 使用硬件能力。

## Layout-owned task policy

H2Loader-managed embedded launcher 不拥有 task-name policy，也不依赖 shared provider 的隐式默认值。ESP launcher 在 `h2_esp_board_runtime_config()`、`h2_runtime_init()` 或直接 PAL task creation 前调用 selected `h2_esp_layout_task_policy_install()`；BK AP launcher 在 board Runtime configuration 前、BK CP launcher在 startup task creation 前调用 `h2_bk_layout_task_policy_install()`。安装失败必须停止 startup。

ESP selected component 位于 `boards/<board>/<target>/layouts/<layout>/`。BK selected AP/CP components 位于 `boards/<board>/bk7258/layouts/<layout>/task_policy/<ap|cp>/`。private board/layout 通过 `layout_files.task_policy` 或 `layout_files.ap_task_policy`/`cp_task_policy` 注入自己的 component，因此 private task name 不进入 GizOS public layout。
