# Bazel 构建与 CI 依赖图

GizOS 使用 Bazel 9.2.0 作为 stable host C/C++ package 以及 ESP-IDF/BK7258/BK3633 firmware 的唯一构建入口，同时描述 CI target、源码 ownership 和依赖关系。macOS 使用 Apple Clang；Linux x86_64 与 Linux aarch64 使用由 `hermetic_cc_toolchain` 固定的交叉工具链。ESP-IDF 与 BK firmware 的底层编译语义仍由各自平台原生 build system 拥有；所有 52 个 ESP、17 个 BK7258 launcher 和 3 个 BK3633 entry 都由 Bazel external rule 调用。

`BUILD.bazel`、`.bzl`、target 命名和 platform variant 同时遵守 [Bazel 代码规范](/zh/coding-styles/bazel)。

## 边界

- 每个 C/C++ library、portable app、Desktop component、board host test 和 host tool 在所属目录的 `BUILD.bazel` 中声明真实可构建的 `cc_library`、`cc_binary` 或 `cc_test`。
- `srcs`、`hdrs`、`data` 和跨 package `deps` 必须显式；不得用 recursive glob、coarse wrapper 或第二套 shadow graph代替语义边界。
- `native_component_src/**` 的每个 SDK component，以及参与相同原生 build graph 的 embedded board、project component 与 firmware launcher，使用 `firmware_native_component` 显式声明原生源码、头文件、构建 metadata 与依赖。该 rule 不产生 host archive；firmware source aspect 到达这个 Provider 后立即停止继续猜测任意 `cc_library` 依赖闭包，并把已声明输入加入对应 native firmware action key。普通 `CcInfo` dependency 只提供 public repository headers；需要由原生 build 编译的 first-party source必须由自己的 `firmware_native_component` 拥有。
- 每个最终 firmware entry 使用且只使用一个 `firmware_lib_component`，其 `deps` 是该 image 选择的全部 Bazel C/C++ libraries。它先用当前目标 C++ toolchain 的 `nm` 检查每个 archive 的 undefined symbols，精确拒绝 newlib standard-stream state `_impure_ptr` 和 `__getreent`，再把验证后的 byte-identical `.a` 交给 `h2_firmware_lib` 原生组件；各 `.a` 不做物理合并，但在最终链接中作为同一个 rescan group 解析循环引用。
- 每个 firmware rule 只从显式 `graph` 收集 component descriptor 与 archive provider。相同 execution-unit/component name 只能有一个 owner，每个 direct source 在一个 firmware closure 内只能有一个 source owner。Runner 生成 invocation-local manifest，ESP-IDF、Armino 和 BK3633 Make 只消费 manifest，不从 CMake/Make 推断 Bazel dependency，也不保留 first-party source fallback。Graph validation 固定 53 个 ESP、17 个 BK7258 和 3 个 BK3633 maintained endpoint。
- Vendor overlay 作为 `third_party/<dependency>.BUILD.bazel` 维护，默认只选择目标实际消费的固定 upstream 文件并暴露 source/header/file group。公开 upstream source 由 `MODULE.bazel` 中 immutable commit archive URL、SHA-256 和 extracted root 唯一固定；repository rule 必须先校验并解压到 external repository，再应用 BUILD overlay、source overlay 和遗留兼容 patch。GizOS-owned upstream 覆盖位于 `third_party/<dependency>_patch/`，其中保存完整 replacement 或新增文件；新增或迁移的 repository rule 只通过显式 `overlay_files` 装配这些文件，不新增 textual patch。GizOS 自有 upstream 的修复必须回到其 owner repository；在 upstream 版本尚未包含修复前，遗留 textual patch 只能作为有上游 Issue 的临时兼容，不能扩展为新的 downstream patch 面。`libs/<library>/BUILD.bazel` 或对应 component 负责把 vendor groups 与 PAL、GizOS API 和 platform dependency 编译成 repository-owned target。明确直接使用纯 upstream contract 时，overlay 可以保留 compiled target；仅用于补齐 upstream portable build 的 config header 可以由 `overlay_files` materialize 到 external repository，但不能 include GizOS header、改变 public API 或取得 platform lifecycle ownership。其中 CMake/configure upstream 可以通过 `rules_foreign_cc` 产生受 Bazel action 管理的 header、link input 和 runtime library。External repository 永远不能反向引用 first-party target label。少量 committed local vendor source 使用独立 local repository path，并监听 source tree；它不能代替公开 archive pin。
- ESP native firmware 必须使用 `esp_idf_firmware`，BK7258 native firmware 必须使用 `bk7258_firmware`，BK3633 firmware endpoint 必须使用 `bk3633_firmware`；不得再用 graph-only `filegroup` 伪装可编译入口。H2Loader 的 ESP/BK7258 最终交付必须另外使用平台无关的 `h2loader_tar_zlib` 消费 native firmware provider。Host Tool 与 Artifact compatibility 是平台选择的唯一 source of truth；BUILD target 不声明自定义 CI tag，也不保存 machine-local SDK path 或 CI runner command。
- 最终 firmware 可以通过 `firmware_version(name, value)` 声明 31-byte 以内、不带 `v` 的 SemVer，并把该 target 传给 native firmware rule 的 `version` attribute。这个 `FirmwareVersionInfo` 是 native image、structured provider、H2Loader package manifest、release metadata 和设备 runtime version 的唯一 source of truth。多个 artifact identity 只有在 product owner 明确 lockstep 时才共享 version target；没有显式声明的兼容 target 继续读取 `//tools/bazel:firmware_version` build setting。Release batch version 只标识一次 immutable assembly，不覆盖显式 firmware version。
- 稳定文件需要 Bazel source ownership 不代表它会进入任何 runtime 或 build action。只服务 repository policy、且没有对应 consumer 的文件使用独立 `filegroup`。
- `generate_graph.py --check` 只验证目录、source ownership、label、artifact rule 和依赖图完整性。依赖边的 source of truth 始终是 checked-in BUILD target。

`//tools/bazel:graph_test` 是这个 workspace audit 的 Bazel owner。它通过测试 source 文件的 realpath 定位当前 checkout，不再依赖 `H2_REPO_ROOT` 环境变量；因此 checkout 的绝对路径不会进入 action environment 或 cache key，也不能指向另一个 checkout 来替代当前 head。

`--config=ci-graph` 固定 Linux x86_64 target/exec platform、`h2_host_os=linux` 与 `h2_ci_graph=true`，并关闭基于调用方 host 的 platform-specific config 自动注入。这样 macOS 上执行的 runner cquery 也分析 Linux CI graph，而不会把 exec-configured host tool 标记为 incompatible；该 config 只用于 graph/runner analysis，不执行 firmware action。
- `third_party/` 和 `x/` 不属于 GizOS stable package tree；stable target 可以通过受控 vendor overlay 消费 `third_party/`，不能依赖 `x/`。

只有 `tools/bazel/esp_idf.bzl`、`tools/bazel/bk7258.bzl`、`tools/bazel/bk3633.bzl` 与 `tools/bazel/jieli.bzl` 可以分别用 Bazel external action 调用原生 `idf.py build`、BK SDK/launcher `make` 与杰理 SDK `make`。它们必须使用显式 target platform、版本化 native repository locator、提交并声明为 action input 的 SDK/toolchain identity、invocation-local build、结构化 provider，以及 `no-sandbox`/`no-remote-exec` execution requirement；不得设置 Bazel 的 `local` execution requirement，因为该关键字会同时禁止 remote cache。成功结果正常进入 local/remote action cache。可选的 `H2_NATIVE_CCACHE_RUNTIME_ROOT` 只在 repository evaluation 阶段定位包含 `runtime.json`、ccache/helper、cache directory 与短期 token 的运行目录；这些绝对路径和 token 不作为 action environment。ESP-IDF 与 BK7258 分别通过原生 IDF/Armino integration 使用 ccache，BK3633 runner 使用 invocation-local GCC wrapper。GCS 中 ESP 与 BK 使用不同 prefix；ccache state 不参与 artifact correctness，也不能代替 Bazel action input、SDK/toolchain identity validation 或 remote action cache。不得复制原生 CMake、Kconfig、partition、bootloader、AP/CP、BIM、Stack 或 linker contract。其它 rule 不得包装 `idf.py` 或 BK `make`。Bazel host fake 验证不能替代真实 S3/P4/BK7258/BK3633 build、firmware matrix 或设备验收。

## ESP-IDF external rule

`esp_idf_firmware` 的 `project` 指向 launcher `CMakeLists.txt`，`project_name` 与 ESP-IDF `PROJECT_NAME` 一致，`target` 只能是 `esp32s3` 或 `esp32p4`。`srcs` 保留 project-local source/action input，`support_files` 显式声明 partition table 等 project directory 外输入，`graph` 使用当前 target configuration 保留 launcher dependency edge。每个 native platform component 在自己的 `BUILD.bazel` 中声明 component name 和原生源码；最终 firmware entry 的唯一 `firmware_lib_component` 从所有 `deps` 的 `CcInfo` linking context 收集主 `.a` 与真实传递静态依赖。Firmware rule 不维护 `libs/**` allowlist。Library component 不能从 `data`、全仓 catalog 或无关 launcher 猜测输入，也不能再次编译相同 source。`--config=esp32s3` 或 `--config=esp32p4` 不替换当前 host platform；compatible `h2loader_tar_zlib` 的 `:package` target 进入 ESP graph，native `:firmware` 由它依赖。launcher 中唯一的 `H2_ESP_TARGET` 仍必须与 rule target 一致，CMake mismatch 在运行 native build 前失败。

ESP32-P4、BK7258、BK3633 与 JieLi pi32v2 的 portable archive 使用 `tools/bazel/toolchains/local_embedded_cc/` 共享 repository/toolchain config。ESP compiler 从 `@h2_esp_idf_tools` locator 选择；BK compiler 从 Bazel 下载并校验的 `@h2_bk_arm_toolchain` 选择，不读取 action environment 中的 path。目标 compile flags、compiler exact version、实际 compiler/assembler/cc1、builtin headers 与 archiver分别进入对应 compile/archive action input。所有 embedded archive 都使用与 native firmware 一致的 `-Os` size optimization；locator 未配置时 repository 仍可加载但注册为 incompatible，不能阻塞无关 host/mobile graph。P4 使用 RV32 `ilp32f`，BK7258 使用 Cortex-M33 hard-float/CMSE，BK3633 使用 ARMv5TE Thumb。Linux、macOS、Android 与 iOS 原生就是 Bazel C/C++ target，不需要 native SDK prebuilt handoff。

调用方由 `.env/devenv` 加载同级 `firmware-devenv`。`--config=esp` 仅用 `--repo_env` 把 `IDF_PATH` 与 `IDF_TOOLS_PATH` 交给 repository rules；tools repository 从固定的 `python_env/idf6.0_py*_env` layout 唯一解析 Python environment。Repository 在发布可 scrub 的 locator 前验证两个 target compiler、Ninja、SDK-owned tool check 与 Python dependency constraints，并监听 SDK 与 tools roots。Native action 只接收 `@h2_esp_idf_sdk` 与 `@h2_esp_idf_tools` locator，不继承这些变量、caller `PATH`、`FIRMWARE_DEVENV_ROOT` 或完整 environment。少数需要 operator 明确选择非敏感 build value 的诊断 image 可以在 rule 的 `cmake_variables` 中逐项 allowlist；只有同名 `--define=NAME=value` 会作为显式 `-D NAME=value` 进入 CMake action。Runner 在 cache miss 执行时再次验证 ESP-IDF commit `662a3be354759d9487bf4b1a629fadb766cb1800` 与相同工具合约，然后将 launcher 复制到独立临时 tree，以内部 `H2_REPO_ROOT` 运行原生 CMake build。

`FirmwareInfo` 返回 target-owned ELF、map、app、bootloader、partition-table image、由同一次构建的 flash arguments 生成并从 `0x0` 烧录的 `combined_factory_image`、完整 flash-files directory、规范化 `flasher_args.json` 和 firmware version。`esp_idf_firmware` 不知道 H2Loader image、role、archive path 或 recovery policy；它的 `DefaultInfo.files` 只包含标准 native 产物，不返回 release provider。所有 required file 必须非空，combined image 必须由 `idf.py merge-bin` 直接写入声明的 native build directory，metadata offset 和 path 必须有效且仍位于本次 build directory。CMake/Ninja intermediate 和 device-operation executable 不进入 provider；Action 不执行 flash、monitor、erase、reset 或 serial 操作。

这个 external action 固定在当前 runner 上 unsandboxed、non-remote-exec 执行，但不设置会禁用 remote cache 的 `local` requirement；`tools/bazel/native_versions/esp_idf_commit.txt` 与 `tools/bazel/native_versions/esp_idf_tool_versions.txt` 都是显式 action input。前者绑定并验证 SDK commit，后者绑定 S3/P4 compiler exact version、S3 archive 使用的 canonical `xtensa-esp-elf-gcc` executable、`xtensa_esp32s3.so` dynamic target configuration，以及 ESP-IDF 6.0 拥有的 Ninja support 与 Python constraints contract；任一版本文件变化都会使 action key 失效。S3 archive wrapper 给 canonical compiler 设置所选 SDK 内的 `XTENSA_GNU_CONFIG`，使它生成与 ESP32-S3 native link 一致的 little-endian ABI；dynamic configuration binary 也属于 compile action input。ESP compiler/tool 版本由该 exact ESP-IDF commit 中的 `tools/tools.json` 选择，CI 从该 checkout 执行 `install.sh`，runner 在调用 `idf.py` 前再次运行 SDK tool 与 Python dependency validation；成功结果可由 local/GCS action cache 复用。GitHub CI 与 Release 从 Bazel catalog 取得 `:package` matrix，构建 package 时传递依赖到 `:firmware`；fake runner contract、graph/catalog test 和真实 S3/P4 matrix 分别验证 action contract、完整性和平台编译。

## BK7258 external rule

H2Loader-managed firmware layout 由 `boards/<board>/<target>/layouts/` 拥有并由 wrapper 按 Board 注入。ESP 没有具名 config profile：每块板拥有一套 canonical `sdkconfig.defaults`，layout 只拥有 partition table、rollback defaults 等 layout 专属项；确有真实差异的变体（例如 Tiga 的诊断串口和 E2E 内存保留）注册为该 board 的独立 layout，firmware target 用 `layout` 属性选择，`config_profile`/`config_profiles` 不再是 ESP rule 接口，能力差异只能由 component graph 表达。BK7258 没有具名 config/GPIO/memory profile：每个 `layouts/<layout>/` 自包含一组 AP/CP defaults、GPIO 选择、`ram_regions.csv` 和 partition metadata，firmware target 用 `layout` 属性选择一个注册 layout；`bk7258_firmware` 只接收显式的 `ap_config`/`cp_config`（按序合并、后层覆盖）与 `ap_gpio`/`cp_gpio` 输入，`config_profile`、`config_profiles`、`gpio_profile`、`memory_profile` 和 runner 的 `--config-layer` 都已删除。App、E2E、产品和 Loader target 都不能复制或覆盖这些 layout 输入，也不能保留 project-local SDK config。最终 SDK 配置按 board canonical defaults、layout defaults 的顺序合并，后层覆盖前层。`ram_regions.csv` 只描述 BK image linker RAM/PSRAM 分配，不属于 OTA geometry，并由拥有它的 layout 声明。

Native firmware rules 在 target graph 上接受 task policy：ESP `esp_idf_firmware` 使用可选的 `task_policy`，BK7258 `bk7258_firmware` 使用可选的 `ap_task_policy` 与 `cp_task_policy`；只有实际使用 PAL task policy 的 target 才传入。H2Loader wrapper 对这些参数 fail closed，并且不再从 `layout_files` 派生 policy。Private downstream target 遵循同一 target-owned contract。ESP image graph 只能包含一个 `h2_esp_target_task_policy`；BK graph 对 AP/CP 各包含一个 execution-unit-specific `h2_bk_target_task_policy`，相同 component name 由 native-component execution-unit key 隔离。每个 target 在自己的 `task_policy/` 中编译完整的 policy source，并且只声明该 App 实际使用的 task name；`//tools/bazel:target_task_policy.bzl` 只统一 component 与 host test 的构建声明，不共享 runtime route 或 resolver 实现。

`bk7258_firmware` 的 `project` 指向 launcher `CMakeLists.txt`，`project_name` 与 SDK `PROJECT` 一致，`target` 固定为 `bk7258`。`srcs`、`support_files` 与 target-configured `graph` 的语义和 ESP rule 相同；compatible `h2loader_tar_zlib` 的 `:package` target 进入 BK7258 graph，native `:firmware` 由它依赖。launcher 中唯一的 `H2_BK_TARGET` 必须与 rule target 一致。全部 17 个 BK7258 launcher 都是 native external build target。

调用方由 `.env/devenv` 提供 `BK7258_PATH`。`--config=bk7258` 仅用 `--repo_env` 将该 locator source 交给 `@h2_bk7258_sdk`；repository rule 验证 exact SDK commit `aa5df964b0f64924ee6d0d2ffd6c3ca6ed59f9ca` 与 tracked cleanliness。`@h2_bk_arm_toolchain` 根据 host 从 Arm 官方地址下载 GNU 10.3-2021.10，先校验 archive SHA-256，再校验完整解压树摘要；macOS arm64 使用 x86_64 archive，并在 repository evaluation 时 fail closed 验证 Rosetta。Native action 只接收两个 locator 和 Bazel Python runtime，不继承 `BK_TOOLCHAIN_ARCHIVE`、`COMPILER_TOOLCHAIN_PATH`、caller `PATH` 或完整 environment。Runner 发布 outputs 前重新验证 SDK identity 与 cleanliness，并确认 GizOS checkout 未变化。

Action 将选中的 launcher 复制到 invocation-local source tree，以只读 `H2_REPO_ROOT` 引用原仓库，以临时 `H2_FIRMWARE_VERSION_FILE` 传递 firmware target 选择的 `FirmwareVersionInfo`，并直接调用 SDK `make`，把 `PROJECT_DIR` 与 `BUILD_DIR` 指向 action-owned 临时目录。它不调用 launcher Makefile；native build 完成后只发布平台固件产物，不判断 Loader role，也不生成 H2Loader recovery bundle。`zero_bk_1_0/apps/h106` 同时执行 source 与 post-link memory contract。Action 不执行 install、flash、reset、serial、network 或设备操作。

`Bk7258FirmwareInfo` 返回 target、firmware version、AP/CP 各自的 ELF、map、app image、managed `app_ab_crc.rbl`、recovery `all-app.bin`，以及至少包含 `partitions.json`、`bk_ota_partitions.json`、`bk_package.json`、`configurationab.json` 的 partition metadata directory。它不包含 H2Loader identity 或 package policy；`bk7258_firmware` 的 `DefaultInfo.files` 只包含这些平台产物，不返回 release provider。所有 required file 必须非空，metadata 必须是 JSON object，symlink 或 resolved path 不能逃出本次 native build directory；其它 update archive 与 SDK intermediate 不进入 provider。

`bk7258_firmware` 与 ESP rule 使用同一个 graph-scoped firmware archive aspect。Runner 只把解析后位于 action input tree 内的主 archive 和 `CcInfo` 传递 archive 写入具名 `H2_BAZEL_PREBUILT_*` 环境；AP component 以 imported CMake target 消费完整 closure，SDK source inventory 不能再包含同一 library 或 third-party source。CP graph 不因 AP dependency 获得这些 archive。

这个 external action 固定在当前 runner 上 unsandboxed、non-remote-exec 执行，但不设置会禁用 remote cache 的 `local` requirement；`tools/bazel/native_versions/bk7258_sdk_commit.txt` 与 `tools/bazel/native_versions/bk_toolchain_archives.txt` 是显式 action input。后者按 host 固定 Arm GNU 10.3-2021.10 URL、archive SHA-256、expanded-tree SHA-256 与 strip prefix，repository rule在发布locator前完成下载和双重校验。Runner调用SDK前再次验证SDK commit与compiler version。任一提交identity或toolchain contract变化都会直接改变action key；生成locator中的绝对路径仅由三个native mnemonic的scrubbing规则排除。成功结果正常读写disk/GCS action cache。

## H2Loader package rule

`h2loader_tar_zlib` 按类型消费标准 `FirmwareInfo` 或 `Bk7258FirmwareInfo`，因此 ESP 与 BK7258 使用同一个包装 rule。它把 provider 声明的 app image 写入 target-specific archive path，并把可选 `package_data` 相对 `package_data_root` 安装到 `data/`；package data 必须由真实 App owner 的具名 target 提供，不能放在 launcher 目录或进入 native firmware 的 `srcs`、`hdrs`、`data`，Loader role 不允许携带 package data。Rule 生成唯一的 `<board>-<image>-<target>.update.tar.zlib` 与 `.firmware.json`；Loader role 在这里从标准平台输出和 Board 的 H2Loader layout recovery config 生成 recovery bundle，并独占 `FirmwareReleaseInfo` 和 `release` output group。Release catalog、ESP/BK7258 CI tag 和公开交付都绑定 `:package`，不绑定内部 `:firmware`。

包装 action 只读取 provider 和 attrs 声明的文件，不访问 SDK、toolchain 或 invocation-local native build tree，因此保留 Bazel 默认 sandbox、remote execution 与 action cache 语义。它不重新解释平台固件格式，也不执行 install、flash、reset、serial、network 或设备操作。

## BK3633 external rule

三个 `bk3633_firmware` target 固定对应 TapDoki production `main`、`log` 和 E2E-owned `//projects/e2e/targets/bk3633_firmware/libco-smoke/tapdoki_v2_0:firmware`。`--config=bk3633` 仅用 `--repo_env` 将 `BK3633_PATH` 交给 `@h2_bk3633_sdk`；action 接收该 SDK locator 与共用的 `@h2_bk_arm_toolchain` locator，不继承 toolchain 或 caller path。Runner 验证 SDK exact commit `c963da8e73440400ee6839b4bfd20ca6e6ec7908`、allroles Stack/BIM input、ARM GCC `10.3.1` 和 build 前后完全一致的 SDK status。Native Make 继续拥有 compile、link、stack、layout、BIM 与 merge 语义，BinConvert 只作为 exec tool 由 Bazel 注入，不递归调用 Bazel。

Rule 通过 native component aspect 将 logical graph 中显式 `firmware_native_component` 的 transitive source、共享 Makefile 与 source-list metadata 纳入 native action key，并通过唯一的 `firmware_lib_component` 接收 host-compiled archive closure。Aspect 只在尚未迁移成显式 Provider 的 board、project entry 和 launcher 边界继续遍历普通 Bazel attributes；到达 `firmware_native_component` 后停止扩张。这样 portable source 或 native build contract 变化都会使三个 entry 中受影响的 action 失效，同时不把 BK3633 build 重写为 host C/C++ graph。

每个 target 固定返回 `firmware/firmware.elf`、`firmware/app.bin`、`firmware/firmware.map`、`firmware/merge-crc.bin` 和 `firmware/manifest.json`。`Bk3633FirmwareInfo`、`DefaultInfo.files` 与 `OutputGroupInfo.release` 暴露相同五个文件；只有 `merge-crc.bin` 是地址 `0x0` 的完整 direct-flash image。BK3633 没有 H2Loader lifecycle，因此不返回 `FirmwareReleaseInfo`、不生成 package，也不进入 GitHub Release asset matrix。Action 使用 invocation-local build/release directory，在当前 runner 上 unsandboxed、non-remote-exec 执行但不设置 `local` requirement，并把 `tools/bazel/native_versions/bk3633_sdk_commit.txt` 与共用的 `tools/bazel/native_versions/bk_toolchain_archives.txt` 作为 action input；SDK repository与BK7258共用的toolchain repository分别验证exact checkout和downloaded toolchain identity。成功结果可进入local/GCS cache，不执行设备操作。

BK3633 graph 只自动收集 native Make 已明确消费的 semantic archive；当前包括
`h2_libco`、`h2_pal` 以及 production image 的 `h2_wolfcrypt` closure。其它 archive
即使出现在逻辑 graph 中也不能在 Make 支持前注入最终链接。过渡性
Component 声明的 native name 必须在同一 firmware action 内唯一。Runner 将 archive
规范化为 `H2_BAZEL_ARCHIVES` Make 列表；共享 Makefile 不得再次编译
对应的 portable 或 upstream source。WolfCrypt 的 ARM/Thumb 与 LTO/non-LTO archive
保持独立 action，并在 native final link 的 group 中解析互相引用；wrapper、retention、
Stack/BIM、layout 和最终 ELF 仍由 native Make 拥有。

## JieLi external rule

`jieli_firmware` 用一个 rule 覆盖杰理各系列：`target = br23`（AC695N）与 `target = wl82`（AC791N）。Firmware entry 只为仓库自己的 portable App 建立，位于 `projects/<project>/targets/jieli_firmware/<image>/<board>/`。`--config=ac695n` / `--config=ac791n` 仅用 `--repo_env` 把 `JIELI_AC695N_SDK_PATH` / `JIELI_AC791N_SDK_PATH` 交给对应 `@h2_jieli_<family>_sdk`，并把 `JIELI_TOOLCHAIN_ROOT`、`JIELI_POSTBUILD_ROOT` 交给共用的 `@h2_jieli_toolchain` 与 `@h2_jieli_postbuild`；这两个 repository 不下载（上游短链不可 pin），而是用 `toolchain_identity.py` 按 `tools/bazel/native_versions/jieli_toolchain_archives.txt` 校验 firmware-devenv 解包出的 expanded tree。SDK repository 验证 `native_versions/jieli_<family>_sdk_commit.txt` exact commit 与 tracked cleanliness。

Runner 把 pinned SDK 子树复制到 invocation-local 目录，用 Bazel 验证过的 `pi32v2/bin` 覆盖 `TOOL_DIR`，并从 `boards/<board>/<chip>/layouts/<profile>/project.mk` 驱动仓库自有 native project。Project 明确拥有 SDK source inventory、flags、linker inputs 与 generated outputs；`tools/bazel/jieli/h2_project_rules.mk` 只追加 Bazel native sources 与 archives，不能 include SDK application Makefile。随后仓库自有 `tools/bazel/jieli/local_post_<target>.sh` 用 Linux `isd_download`/`fw_add`/`ufw_maker` 本地出包。Portable archive 由 `@h2_jieli_pi32v2_cc_toolchain` 编译为非 LTO ELF 对象，SDK source object 为 LTO bitcode。每个 target 固定返回 `firmware/firmware.elf`、`symbols.txt`、`jl_isd.bin`、`jl_isd.fw`、`update.ufw` 与 `manifest.json`。

pi32v2 compatible graph 覆盖所有无平台门控的 `cc_library`；clang 4.0.1 编不了的目标用 `PI32V2_UNSUPPORTED_ARTIFACT_COMPATIBILITY` 显式退出并注明原因（见 [JieLi Components](./components/jieli)）。工具链只有 Linux x86_64 二进制，因此 rule 的 compatibility 同时要求 `h2_firmware_target` 与 `h2_host_os=linux`：Linux CI 与 Linux 开发机原生执行，macOS host 上这些 target 被标为 incompatible 并跳过，macOS 开发者在 Linux dev container 内运行整个 Bazel；不在 action 内包装 `docker run`，runner 也在触碰 SDK 前拒绝非 Linux x86_64 host。

## Library 与 artifact graph

每个 stable library、App、Host Tool 和 Artifact 在自己的 `BUILD.bazel` 中声明真实 target、源码和依赖。Graph validator 从目录结构、rule kind 和 dependency graph 检查这些边界；不维护 tag、checked-in label list、生成 inventory或固定 target count。

Host Tool 与 Artifact compatibility 决定 target 是否进入当前平台 graph。Validator 拒绝缺少 BUILD ownership、缺少 required library/component rule、错误 artifact rule、缺失 target 和没有 BUILD owner 的 stable file。

从 repository root 构建和测试当前 config 的完整 compatible graph：

```sh
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

每个 CI execution class 直接请求 `//...`。Build task 构建完整 compatible graph，并在普通 CI 中使用 `--remote_download_outputs=minimal`，不为没有后续 artifact consumer 的 cache hit 物化全部 top-level output；需要读取产物的 validation 必须用 focused target 显式物化。Test task 统一使用 `--build_tests_only --remote_download_outputs=minimal`，只构建自动测试及其 Bazel 依赖闭包，并只物化本地执行实际需要的 remote output。Platform compatibility 跳过其他平台，Bazel repository/disk cache 根据完整 action key 复用未变化的下载、生成、编译、链接和测试输出。CI 不再计算 base-to-head affected target，也不传递 exact target list。

Native firmware action 向 Bazel 声明每个 action 使用 4 CPU 和 4 GiB memory，并把底层 ESP-IDF/BK build 的并发限制为 4。Bazel 根据 runner 的可用资源自动决定同时运行多少个独立 firmware action；升级单台 runner 会自然提高 launcher 并行数，不需要为每个 launcher 创建 GitHub matrix job。

CI 按 execution class 通过 repository variable 选择 runner。GizOS 保持与 Firmwares 相同的变量接口和 class assignment，但 runner label 必须属于 `GizClaw` 组织可用的 runner pool：

- `FIRMWARE_LINUX_RUNNER` 服务 ESP32-P4、BK3633 与 CI required aggregator；
- `FIRMWARE_LINUX_4CORE_RUNNER` 服务 Android Build/Test、Test coverage 与 K4B；
- `FIRMWARE_LINUX_8CORE_RUNNER` 服务 Linux Build/Test、ESP32-S3 与 BK7258；
- `FIRMWARE_MACOS_RUNNER` 服务 macOS Build/Test；
- `FIRMWARE_WINDOWS_RUNNER` 服务 Windows Build/Test。

当前 `GizClaw` 组织不提供 Larger Runner，Linux variables 使用标准 `ubuntu-24.04`，macOS variable 使用标准 `macos-15`，Windows variable 使用标准 `windows-2025`。这些变量保留 execution class 的稳定接口；组织获得 Larger Runner 后可以只更新 variable value，不修改 workflow。iOS Simulator 继续使用标准 `macos-15`。

Linux runner 的 system dependency 安装是后续 SDK、toolchain 与 Bazel cache 网络访问的 fail-fast health gate。APT 不在当前 runner 内重试，单次 HTTP/HTTPS 等待最多 30 秒，整个 step 最多 5 分钟；超时或下载失败直接使当前 job 失败，由新的 workflow run 使用干净 runner 重试。

平台配置为：

```sh
bazel test --config=macos_arm64 //libs/... //libs/pal/providers/desktop/... //projects/.../cc_binary/...
bazel test --config=linux_x86_64 //libs/... //libs/pal/providers/desktop/... //projects/.../cc_binary/...
bazel build --config=linux_arm64 //tools/bazel/tests/toolchain_smoke:cross_artifact
bazel build -c opt --config=kickpi_k4b  //projects/example/targets/cc_binary/display/kickpi_k4b:display  //projects/example/targets/cc_binary/mp4-player/kickpi_k4b:mp4_player
bazel test --config=ios_sim_arm64 //projects/example/targets/ios_application/tap-reset:package_validation_test
bazel test --config=android_arm64 --platforms=//tools/bazel/platforms:linux_x86_64 //projects/example/targets/android_binary/tap-reset:package_validation_test //projects/example/targets/android_binary/mp4-player:package_validation_test
```

## Windows compatible graph

Windows x86_64 已提供原生 Bazel Build/Test execution class。根 `MODULE.bazel` 通过 `rules_cc` 的 `cc_configure_extension` 暴露 host-local `@local_config_cc`；`--config=windows_x86_64` 选择 `//tools/bazel/platforms:windows_x86_64` 作为 target/execution platform、选择 `@local_config_cc//:cc-toolchain-x64_windows`，并启用 `compiler_param_file` 与 runfiles tree。Windows 使用 runfiles tree 时必须同时通过 startup option 启用符号链接支持；生成的 `.js`、`.wasm` 与 archive 继续作为测试的显式 `data` 依赖，由 `--remote_download_outputs=minimal` 在本地测试动作需要时按需从远端缓存物化，不能用 `no-remote-cache`、`--remote_download_outputs=all`、输出路径 allowlist 或 POSIX-only compatibility 掩盖依赖错误。该配置只在原生 Windows runner 上成立，不是 Linux/macOS 到 Windows 的 cross toolchain。ESP32-S3 等只支持 Linux/macOS execution host 的全局注册 toolchain repository 在 Windows 上必须仍可加载，并把自身声明为 incompatible，不能在无关 Windows target 进入 toolchain resolution 前使整个 module graph 失败。

一方 C/C++ target 通过 `//tools/bazel:cc_options.bzl` 选择语言标准与 warning policy：GNU/Clang 使用 `-std`/`-W*`，MSVC 的 C11 target 使用 `/std:c11` 与 `/experimental:c11atomics`，C++ target 使用 `/std:*`，两者使用 `/W4`/`/WX`。BUILD target 不得直接写全局 `-Wall`/`-Wextra`/`-std=c11`/`-std=c++17`；只对 GNU/Clang 成立的 suppression 通过 `h2_gnu_only_copts` 隔离，避免 MSVC 把它们解析成无效 `/W*` 参数。

Required CI 的 `windows-compatible-graph` job 通过 `FIRMWARE_WINDOWS_RUNNER` 选择 Windows Server 2025 runner，在同一 Bazel server 与 output base 中依次请求 `bazel build --config=windows_x86_64 //...` 和带统一 test options 的 `bazel test --config=windows_x86_64 //...`。Build 使用 minimal remote output 下载，Test 复用 Build 已完成的 analysis 与 action state。Bazel 依据 `target_compatible_with` 跳过不属于 Windows 的 target；自动测试不声明 tag，只有需要外部环境的 opt-in 测试声明 `manual`。Apple/mobile rule 的 transition 在 compatibility 判定前就会请求平台 toolchain，因此 Windows config 只通过 `--deleted_packages` 排除这些已有独立平台 CI 的 artifact package；普通 target 仍必须用 compatibility contract，不维护 Windows target allowlist。

`//tools/bazel/tests/toolchain_smoke:windows_exe_smoke` 是标准 `cc_binary`，对应的 `windows_exe_smoke_test` 保证 Windows Test graph 至少执行一个真实 Win32 测试。它用 MSVC C++17 warning policy 编译 Win32 source，显式链接 `bcrypt.lib`，并调用 `BCryptGenRandom`。完整 Build/Test 后只对这个 focused target 使用 `--remote_download_outputs=toplevel`，再验证 declared `.exe` 的 PE signature、x86_64 COFF machine、PE32+ magic 和运行成功 marker，防止仅 analysis、cache metadata 或空 graph 绿灯。Windows CI 使用经 SHA-256 校验的 Bazel 9.2.0 Windows executable，并以 `--lockfile_mode=off` 规避 `emsdk` extension 在 Windows 与现有 macOS/Linux host 间的 transitive digest 差异；现有 host 仍以 `bazel mod deps --lockfile_mode=error` 验证 committed lockfile。`ci-required` 同时要求现有 matrix 和 Windows Build/Test 成功。

Focused native Windows validation 为：

```powershell
bazel.exe build --config=windows_x86_64 //tools/bazel/tests/toolchain_smoke:windows_exe_smoke
bazel.exe cquery --config=windows_x86_64 --output=files //tools/bazel/tests/toolchain_smoke:windows_exe_smoke
bazel.exe build --config=windows_x86_64 //...
bazel.exe test --config=windows_x86_64 //...
```

完整 compatible graph 只证明已声明 Windows-compatible 的 target 可构建且自动测试可执行，不等于 Windows PAL、Runtime、Desktop App、设备能力或 distribution 已支持。#835 负责在该 execution class 上实现 Windows PAL。

macOS CI 和 Release 明确选择 Xcode 16.4，并通过 `macos_arm64` config 把 `MACOSX_DEPLOYMENT_TARGET` 固定为 13.0，使 Apple Clang 和最低系统版本不随 runner 漂移。macOS arm64 与 Linux x86_64 对 SDL3、PortAudio 和 FFmpeg 使用相同 immutable archive pin，并从 Bazel external repository 进入 `rules_foreign_cc` action；配置、编译、安装目录和 runtime shared library 都属于 Bazel graph，不读取旧的 dependency-prefix repository environment、Homebrew prefix 或发行版 SDL、PortAudio、FFmpeg development package。coreHTTP、llhttp 与 wolfSSL 同样从固定 archive 静态进入 first-party target；Linux 不安装或链接发行版 libcurl。Linux 只把 X11、ALSA 等 OS service development boundary 作为显式 host dependency。Linux x86_64 CI 从 fresh output base 禁止 repository download 后重建 representative Desktop binary，并用 `tools/bazel/linux_runtime` 验证完整 `ldd` closure；allowlist 不得包含 curl、wolfSSL、OpenSSL、coreHTTP 或 llhttp。Linux aarch64 smoke 仍只验证 toolchain 生成目标架构 ELF、符号与重复构建 checksum，不承担这些 upstream 的 source build。

Repository checks 只在 macOS host execution class 执行一次，并统一使用 `DARWIN_HOST_TOOL_COMPATIBILITY`。它们检查仓库结构、公开命令面、Guide 构建与信息架构、H106 E2E 清单与报告、i18n committed catalog 与 UI literal、committed PNG descriptor、GizClaw source contract 和 firmware memory contract；Linux、Windows、Android 与 coverage execution class 不重复执行。当前 CI 和本地标准命令使用 macOS arm64 config，开发者可通过 `bazel test --config=macos_arm64 //...` 执行同一组检查。C/C++ 编译、PAL、package、archive 与平台行为测试不属于 repository checks，仍按各自 target compatibility 在对应 execution class 验证。

Linux Host coverage 使用独立的 `test_coverage` config。它只在 Linux x86_64
上选择固定的 `toolchains_llvm` 1.8.0 与 LLVM 16.0.0，通过 Bazel coverage
map、`llvm-profdata` 和 `llvm-cov` 生成 combined LCOV；普通
`linux_x86_64` build/test 仍选择 `@zig_sdk`，coverage toolchain 不注册为默认
toolchain。Ubuntu 24.04 runner 从 Ubuntu 22.04 security archive 安装经过
SHA-256 固定的 `libtinfo5` compatibility package，满足 LLVM distribution 的
legacy runtime dependency；`lcov`/`genhtml` 仍由 runner package manager
提供，不从 `PATH` 发现 compiler、`llvm-profdata` 或 `llvm-cov`。Coverage
config 显式定义 Linux `_GNU_SOURCE`，并只把 LLVM toolchain 自身
`-stdlib=libc++` 在 compile action 中产生的
`unused-command-line-argument` 从仓库 target 的 `-Werror` 中排除，其他
warning 仍为 error。

`make bazel-coverage-report` 使用同一个 configured graph 查询全部 compatible、
非 `manual` test，以及全部 compatible `cc_library` 和 `cc_binary` target；
随后执行 `bazel coverage --config=test_coverage --combined_report=lcov
--cache_test_results=no <compatible-test-labels...>`。Coverage instrumentation
filter 由这些 target 声明源码所在的 repository directory 动态生成。因此新增
target 不需要加入 coverage allowlist：target compatibility、`manual` test tag
和 target 声明的 `srcs`/`hdrs` 是 source of truth。Remote action cache
继续复用 instrumented compile、archive 和 link
action；test result cache 被禁止，BEP 中出现 cached test 同样使报告失败。

JSON BEP 生成 `tests.json`，逐个对账 expected test 的 terminal status、run、
attempt、shard、duration 和 cached action。Combined LCOV 排除 test source、
generated source、external/vendor source 和 H106 Host adapter，再按 target
直接声明的 `srcs`/`hdrs` 分别生成
`targets.json`。每项使用 `cc_library/<name>` 或 `cc_binary/<name>` 展示，并保留
Bazel package 以区分重名 target。Target 状态是
`measured`、`uncovered` 或带原因的 `not-applicable`；`uncovered` 没有可用的
LCOV denominator，因此不会伪造 `0%`。Line、function 和可用时的 branch
percentage 只表示 measured scope，summary 另外显示 measured/eligible target
数量。

固定输出位于 `build/coverage/test/`：`coverage.dat`、`targets.json`、
`tests.json`、`summary.md` 和 `report/index.html`。Actions 总是尝试把
`summary.md` 写入 job summary，并把已有文件上传为私有 `test-coverage`
artifact。测试或报告失败时 partial artifact 只用于诊断，job 仍失败；旧报告、
cached test、H106-only fallback 和公开 coverage service 都不能替代当前 run 的
evidence。

## Upstream archive 获取

CI checkout 使用当前 event exact head 且不初始化 submodule。`MODULE.bazel` 为每个公开 upstream dependency 声明 immutable commit archive URL、SHA-256 和 extracted root；依赖实际消费的 nested upstream source 同样以原 submodule path、exact commit archive、SHA-256 和 extracted root 显式声明，不递归 Git checkout。`vendor_repositories` 下载到 Bazel repository cache，验证 digest 和 archive layout 后才创建 `@h2_vendor_*` repository。缺失 archive、digest mismatch、错误 extracted root 或无法应用的 patch/overlay 必须在编译前 fail closed。

Repository cache 只保存由 Bazel 按 digest 索引的下载内容，不保存 expanded external repository、Git config、credential、hook、worktree、LFS payload 或 build output。Cache miss、eviction、corruption 或 restore/save error 只会触发重新下载，不能跳过 integrity validation、Bazel analysis 或 consumer。`third_party/` 中碰巧存在同名目录不能改变 archive resolution。

`kickpi_k4b` config 固定 Linux x86_64 execution platform 与 ARMv7 GNU EABI hard-float target，并固定使用 `-c opt`。Bazel repository rule 从 Arm 官方地址下载 `gcc-arm-8.2-2018.11-x86_64-arm-linux-gnueabihf`，校验 SHA-256 并把完整 compiler/sysroot distribution 注册为 C/C++ toolchain；C 与 C++ builtin include 都来自该 distribution，compile/link action 不读取 `K4B_TOOLCHAIN_ROOT`、ambient `CC` 或 `PATH`。MP4 Player 另外通过 `--repo_env=K4B_CEDARX_INCLUDE_DIR` 与 `--repo_env=K4B_CEDARX_LIB_DIR` 把 `firmwares-devenv` provision 的 CedarX headers 和目标 image shared library closure 映射为 `@h2_k4b_cedarx_sdk`，由 `cc_import` 参与同一 Bazel link graph；AAC-LC decoder 则由 Bazel 使用固定 URL/SHA-256 下载 FDK-AAC 2.0.1 source 并交叉编译 decoder-only source closure。K4B board、launcher 与 CedarX production target 必须声明 Linux/ARMv7 `target_compatible_with`。没有 CedarX env 时真正分析、编译或链接 private CedarX consumer 必须 fail closed。

## CI Cache

Bazel 的 repository cache 固定在 `~/.cache/bazel/repository`，保存带 integrity 的 module、toolchain 和 external repository 下载；disk cache 固定在 `~/.cache/bazel/disk`，保存本机 compile、link、test、foreign C/C++ build 和生成 action 的 content-addressed output。本机默认只使用这两个 cache，不自动探测 CI、hostname 或 GCP credential，也不访问 GCS。

GitHub 外层 repository cache key 只包含 schema、runner OS/architecture、Bazel version 和 `MODULE.bazel`/`MODULE.bazel.lock` digest；相同 dependency closure 不按 PR head 或 job scope 复制。Repository download cache 继续由 Actions cache 保存；Bazel action output 使用共享 GCS remote cache。Remote cache 已配置时，普通 CI 显式设置 `BAZEL_DISK_CACHE_MODE=off`，不在 ephemeral runner 上把相同 action output 同时保存在 output tree 与 job-local disk cache。Linux、macOS、Android 与 Windows 的 Build/Test 在各自的一台 runner 上顺序执行并复用同一个 Bazel server 和 output base。Android Test 的 package validation 会切换 top-level target platform 并形成第二份 configured graph，但兼容 dependency action 仍能按 content-addressed action key 复用前序 Android Build 的结果。iOS Build/Test 继续作为独立 task 并行执行。所有 execution class 仍通过相同 GCS namespace 共享已完成上传的 content-addressed output。Remote cache 未配置时不设置该 mode，每个 job 使用 job-local disk cache fallback。

Windows Build 与 Test 虽然通过 PowerShell 直接调用 Bazel，也必须使用同一套 GCS remote cache、Workload Identity Federation、repository cache restore/save 和无 remote 时的 job-local disk cache fallback；不能因为没有经过公共 Make 入口而退化为固定冷构建。所有普通 CI Build 和 Test task 都使用 `--remote_download_outputs=minimal`；Test 另外统一使用 `--build_tests_only`。Bazel 仍分析选中 target 并自动解析完整依赖闭包，但不预先物化本地执行不需要的 remote output。测试运行时输入必须通过 Bazel target 的 `data`、`deps` 或 toolchain contract 声明；需要检查 top-level artifact 的 step 必须只为对应 focused target 显式请求 `toplevel`，不能用 `--remote_download_outputs=all` 掩盖缺失依赖。

GitHub Actions 只在 repository variable `BAZEL_REMOTE_CACHE_URL` 完整配置时启用 remote cache；URL 必须是 `https://storage.googleapis.com/<bucket>/firmwares`。Workflow 通过 Workload Identity Federation 生成 Application Default Credentials。同仓 pull request、`main`、manual CI 与 Release 都使用 Writer；pull request job 会校验 head repository 与当前 repository 相同。Pull request 的 `opened` 和 `synchronize` event 直接触发 CI，并为每个 distinct head 运行一次完整 platform matrix；reopen、title/body edit 和 draft-state change 不重复构建未变化的 head。Ownership approval 只负责 merge eligibility，不能延迟或调度 build。Bazel 保持默认异步上传，成功 action 完成后即可填充共享 cache，失败 action 不会缓存。变量完全未配置时 server job 明确回退到 job-local disk cache；只配置一部分变量时 fail closed。本机如需主动使用同一 cache，必须显式提供 URL、ADC 和 `BAZEL_REMOTE_CACHE_MODE=read|write`。

Consumer repository 固定使用以下四个非敏感 variables，不保存 Service Account key：

- `BAZEL_CACHE_PROJECT_ID`；
- `BAZEL_REMOTE_CACHE_URL`；
- `GCP_WORKLOAD_IDENTITY_PROVIDER`；
- `BAZEL_CACHE_WRITER_SERVICE_ACCOUNT`。

ESP CI 与 Release 的 `~/.espressif` 使用相同的 OS、ESP-IDF commit 与 target-set identity，不再按 build input hash 或 release job 复制 1 GiB 级 toolchain snapshot。PR 与 Release 只 restore；受保护的 `main` miss 时保存一份 seed。旧 CI/Release prefix 只在 schema 迁移期作为 restore seed，不能继续生成旧 key。

开发机 disk cache 最多保留 4 GiB 且淘汰 14 天未使用的 entry；CI 不启用该 cache。GCS Bucket 对 cache object 使用统一 30 天 creation-age Delete lifecycle；读取不会刷新 object age，删除后的首次 miss 由 Bazel 正常重建。Bucket、生命周期、WIF、Reader/Writer 与 namespace URL 由 GizClaw Deploy shared Infrastructure 拥有，GizOS workflow 只消费非敏感 outputs。

Repository cache 或 remote action cache 的 miss、eviction 和生命周期删除只能降低构建速度，不能跳过 Bazel analysis、build、test、package 或 release validation。身份、URL 或部分变量配置错误必须失败，不能无提示地把预期 remote-cache job 当作成功。本地 disk/repository cache、GCS remote cache 和 ESP tool cache 都不能成为 source、release artifact、test result 或设备验收的 source of truth。

Graph test 对目录、label、rule kind、artifact identity 和依赖边做校验，并拒绝 `test_suite`；它只扫描 Git 已跟踪或未跟踪但未忽略的一方 `BUILD.bazel`/`.bzl` 文件，不能递归进入 `bazel-*` convenience link、external repository 或 cache output。Build 命令直接请求完整 compatible graph，Release 从 artifact rule/provider 查询交付目标，不维护第二份聚合名单。普通 test target 不声明 tag；依赖外部服务、真实设备或人工环境准备的 E2E test 只声明 Bazel 特殊 tag `manual`，由 Bazel 的通配 target pattern 语义自动排除。专用 E2E 命令直接请求 exact label，不维护 tag filter。CI 的 `BAZEL_CONFIG` 决定唯一 execution class；Linux、macOS、Android 与 Windows 在一个 job 内依次请求 Build/Test，iOS 保留独立并行的 Build/Test task。

## 下游 Bzlmod consumer

产品仓库通过名为 `gizos` 的 Bzlmod module 消费公开 target，并统一使用 `@gizos//...` label。GizOS 自己作为 root 时使用的具名 Node toolchain extension 和 registration 标记为 `dev_dependency`；它在 GizOS root build 中保持生效，在 GizOS 作为 dependency 时不替下游选择 root toolchain。GizOS 的官方 `go_sdk` extension usage 固定 Go 1.24.12，使公共 Go host tool 与 Gazelle repository tool 在完整 module graph 中选择兼容 SDK；GizOS 同时通过 `@gizos//tools/bazel:go_host_toolchains.bzl` 公开 root-registerable host toolchain extension。Toolchain registration 只对 root module 生效，因此 consumer root 必须调用这个 GizOS-owned extension、导入 `gizos_go_toolchains` 并注册 `@gizos_go_toolchains//:all`；consumer 不直接声明或 load `rules_go`，也不自行选择 Go 版本、archive 或 digest。Zig extension 同样只为实际 root module 生成 toolchain repository，因此使用 GizOS C/C++ graph 的下游 root 必须声明自己的同一 extension usage 和 registration。下游 root 同时负责 platform config、构建环境和最终产品 artifact，GizOS 仍拥有 public library、PAL provider、native component、firmware rule 和 H2Loader package rule。

公开 `.bzl` 中指向 GizOS-owned tool、config setting 或默认输入的 label 必须稳定解析到 `@gizos//...`，不能用只在 root repository 成立的 `@//...`。Consumer-owned `srcs`、`data`、board、App 与 launcher 继续由调用方显式传入，GizOS rule 不能反向取得产品 ownership。

`make bazel-test-downstream-consumer` 在临时 root module 中通过 `--override_module=gizos=<checkout>` 构建 Runtime、native component、firmware library composition 和 H2Loader package fixture。该验证不替代 GizOS 自身的完整 compatible graph，也不把本地 checkout override 当作发布 pin；正式 consumer 必须使用已发布版本或 immutable revision 与完整性校验。

`NATIVE_TARGETS` 只接受 `//package:target` exact label，不提供 package path shorthand、recursive pattern 或兼容转换。CI 显式传入 `H2_BAZEL_CONFIG`；本地未传时，Make entry 按当前 macOS arm64、Linux x86_64 或 Linux arm64 host 选择已有 config。

## CI execution contract

Pull request、main push 和 workflow dispatch 使用同一份静态 platform matrix。K4B 在 #794 关闭前仍自动执行完整 build，但作为 non-blocking execution class：失败在 CI 中保持可见，却不阻塞 `CI required`；显式本地 `kickpi_k4b` config 保持不变。Pull request checkout exact head，main push 与手动运行 checkout event revision。Linux、macOS 与 Android 各使用一个 matrix job 顺序执行 Build/Test，复用同一个 Bazel server 和 output base；Windows 使用对应的单一 native job。iOS 继续用两个独立 task 并行执行：

```sh
make bazel-build BAZEL_CONFIG=<config>
make bazel-test BAZEL_CONFIG=<config>
```

Linux 与 macOS Test 执行完整 compatible host test graph；Android 在同一个 job 中先 Build，再执行对应 package validation；iOS 的独立 Test task 同样只执行 package validation，因为普通 host `cc_test` 不能作为移动平台二进制直接在 runner OS 上执行。Firmware 和 KickPi class 只有 Build task；Test coverage 使用独立的 `make bazel-coverage-report`。Release workflow 保持独立的 closed artifact slice，不通过普通 CI command 生成 release bundle。

Bazel 必须分析当前 config 下的完整 `//...` graph。Platform compatibility 决定 target 是否属于该 execution class；Bazel 特殊 tag `manual` 只用于阻止通配 target pattern 自动选择手工测试，不需要 tag filter。Repository cache、disk cache、persistent action cache 和 Skyframe 可以减少实际下载与 action execution，但不能成为省略 graph、build、test 或 coverage 的理由。

Host tool compatibility 必须同时匹配 runner OS 与 CPU：当前 Linux host class 是 x86_64，macOS host class 是 arm64。Cross config（例如 KickPi ARMv7 或 Linux arm64）必须通过准确的 `target_compatible_with` 跳过顶层 host Python/tool targets，包括只在 host 上生成 filesystem image 的 `//tools/mklittlefs:mklittlefs`；这些 target 作为其他 rule 的 tool dependency 时由 exec transition 在 runner platform 构建，不能要求 cross target Python/C++ toolchain。Web artifact wrapper 与 validation 只兼容 Linux x86_64 或 macOS arm64 host，Web C/C++ target 只兼容 Emscripten transition 使用的 `wasm32` target platform，不能在普通 host、mobile 或 firmware platform 上直接编译。

ESP32-S3、ESP32-P4、BK7258 与 BK3633 config 直接自动选择各自的全部 firmware endpoint。每个 native firmware action 声明 4 CPU、4 GiB memory，并向底层 native build 传递 4-way parallelism；Bazel 在单台 runner 上按真实可用资源并发调度不同 launcher。Runner 规格增大时并行 launcher 数自动增加，不需要维护 affected target list 或为每个 launcher 展开 GitHub matrix。

CI 为 ESP32-S3、ESP32-P4、BK7258 与 BK3633 创建同一固定 layout 的 `H2_NATIVE_CCACHE_RUNTIME_ROOT`，其中每个 target namespace 使用最多 1 GiB 的一级 cache，并以同一 GCS bucket 的 `ccache/esp` 与 `ccache/bk` 作为二级remote storage。Bazel Remote Cache继续独占`firmwares` prefix；native ccache不使用Bazel layout，也不通过GitHub Actions cache restore/save object directory。`runtime.json`相对引用ccache/helper、cache root与短期token；repository locator绝对路径只在三个native firmware mnemonic中由Bazel 9.2 scrubbing排除，其他input与action不受影响。S3/P4和BK7258/BK3633分别使用target namespace；源码、compiler content、flags、included config与generated header仍由ccache compile key判定。PR、`main`、manual与Release使用相同Writer，并在firmware build前刷新短期token。含credential的ESP build通过单个`H2LOADER_WIFI_CREDENTIALS` JSON action environment接收`ssid`与`password`，并只读共享cache。

Execution matrix 不使用 fail-fast；任一 execution class 失败后，其他 class 仍运行到终态，以便一次收集完整的平台问题。`ci-required` 使用 `if: always()` 汇总 matrix；required execution class 的 failure、cancelled 或 skipped 都必须让 required gate 失败。

## 更新和验证

增加、删除或移动 directory、dependency、ESP/BK7258/BK3633 launcher、global runner input 或 upstream archive pin 时，更新对应 BUILD target 与 compatibility，再运行：

```sh
bazel test //tools/bazel/...
bazel test //tools/bazel:bk3633_runner_test //tools/bazel:bk7258_runner_test //tools/bazel:esp_idf_runner_test
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

Review 必须覆盖所有修改到的 platform config、native runner、workflow、cache contract 和 public Make command；证据必须对应最终 head。
