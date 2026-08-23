# Bazel 代码规范

这份规范约束仓库拥有的 `BUILD.bazel`、`.bzl` 和 `third_party/*.BUILD.bazel` overlay。Bazel target 同时是可构建单元、源码 ownership 和完整 CI 依赖图，名称不能只为某个调用点提供方便。

## Target 边界

- 一个 `cc_library` 对应一个可独立说明的 API、实现或 build variant。不能按消费它的 App 名复制第三方或公共 library，例如不能创建 `lvgl_tap_reset`。
- 默认 target 表示所有 consumer 共享的唯一实现。只有 source set、compile definitions、配置 header、toolchain compatibility 或直接 dependency graph 确实不同时，才拆 build variant。
- 平台 family variant 使用 `_embed`、`_desktop`、`_mobile` 或 `_web` 后缀。后缀描述产物运行环境，不描述当前 App、board 或 launcher 名；没有差异时保留无后缀 target，不建立只转发到同一实现的别名。
- 同一份 portable App source 可以由 `app_mobile`、`app_desktop` 等 target 分别编译，以绑定不同的 LVGL、PAL 或 toolchain variant。App source 仍保持 portable；不同的是 build graph 和最终 artifact，不是业务实现。
- `_web` target 必须由真实 WebAssembly toolchain、配置或 dependency graph 支撑。不得把 native `cc_library`、shell script 或 `genrule` 命名为 `_web` 来代替 Emscripten/WebAssembly 构建。

## 依赖与 ownership

- Portable App 和 `libs/` 不能反向依赖具体 artifact entry。Platform wrapper 依赖 portable API；最终 platform target 同时链接 wrapper 与对应 App/library variant。
- `third_party/*.BUILD.bazel` overlay 默认只暴露上游 source/header/file group。由于 `filegroup` 不能传播 C/C++ include root，overlay 可以用不含 `srcs` 和 first-party dependency 的 header-only `cc_library` 表达 header group。
- 当仓库通过 `libs/<library>` 或 component 集成 upstream 时，由 first-party owner 把 vendor groups 编译成默认 target 或 `_embed`、`_desktop`、`_mobile`、`_web` variant，并选择 PAL 和 platform dependency。只有 repository guide 明确允许 consumer 直接使用纯 upstream contract，且编译规则不需要 GizOS API/source/platform dependency 时，overlay 才可以直接定义编译 target。仅用于补齐 upstream portable build 的 config header 可以通过 repository rule 的 `overlay_files` materialize 到 external repository；它不能 include GizOS header、改变 public API 或取得 platform lifecycle ownership。
- 依赖方向必须是 `consumer -> first-party owner -> vendor`，或在明确允许直接消费纯 upstream contract 时为 `consumer -> vendor`。External repository 在任何情况下都不能通过 `load`、`srcs`、`hdrs`、`data`、`deps` 或 toolchain attribute 反向引用 `@gizos//...`、`@//libs`、`@//native_component_src`、`@//projects`、`@//boards` 或 `@//tools`；repository rule 在创建 external repository 前复制的显式 `overlay_files` 不构成这种反向 target dependency。
- `srcs`、`hdrs`、`data` 与跨 package `deps` 显式列出。除目标 SDK adapter 已有明确例外外，不用 recursive glob 模糊 package 边界。
- 由 ESP-IDF、BK7258 或 BK3633 原生构建系统编译的 `native_component_src/**`、embedded board、project component 与 firmware launcher 使用 `firmware_native_component`，不能用 `cc_library` 假装由 Bazel C/C++ toolchain 编译，也不能用 recursive `filegroup` 代替组件依赖。每个最终 firmware entry 另用唯一一个 `firmware_lib_component` 聚合全部 Bazel library。真实 compile、link、SDK config 与 lifecycle 仍由对应原生构建入口拥有。确实由 Bazel C/C++ action 编译的 portable library、host helper 和 test 继续使用 `cc_library` 或 `cc_test`。
- Platform config 通过 Host Tool 与 Artifact compatibility 选择完整 graph；不得用自定义 tag 或 checked-in target list 维护第二份平台、library 或 artifact inventory。
- 普通 test target 不声明 tag；依赖外部服务、真实设备或人工环境准备的 E2E test 只声明 Bazel 特殊 tag `manual`，由 Bazel 的通配 target pattern 语义自动排除。手工入口直接请求 exact label，不维护 tag filter，也不使用 `test_suite` 聚合测试。
- Native SDK external rule 必须明确 SDK/target platform、environment allowlist、source isolation、declared output provider 和失败合同。外部 SDK 当前由仓库内提交的版本文件标识，版本文件必须是 action input；action 必须在当前 runner 上执行、使用 `no-sandbox` 与 `no-remote-exec`，但不得使用会同时禁止 remote cache 的 `local`、`no-cache` 或 `no-remote-cache` execution requirement。runner 只读取 allowlist environment，不得 activation、自动发现 fallback、修改 source/SDK 或产生设备副作用。BK7258 provider 必须保持 AP、CP、managed app、recovery 与 partition metadata 的 owner/type 边界，不能用一个不透明 output directory 代替结构化 artifact。
- Native SDK external rule 可以通过 `H2_NATIVE_CCACHE_RUNTIME_ROOT` repository locator 使用 `ccache` 加速 action 内部编译。该 root 固定包含相对路径描述的 `runtime.json`、ccache/helper、cache directory 与可选短期 token；各字段不得分别进入 action environment。Runner 按 firmware target 隔离 namespace，并将 invocation-local root 下的绝对路径归一化、排除随机 working directory hash，使同一 target 的不同 launcher 和 workflow 可以共享 object。SDK、toolchain、source、configuration 与 generated header 仍由 `ccache` 计算 compile key。可选 GCS remote storage 必须完整配置；token 内容不得进入 Bazel action environment、action key、日志、artifact 或 cache object。ESP 与 BK 使用独立 GCS prefix，且都不能使用 Bazel Remote Cache 的 namespace 或 layout。接收 credential 或其他 secret build input 的 action只能读取共享 compiler cache。`ccache` 只影响性能，cache 缺失、清空或未配置时必须产生相同的原生构建行为与 public artifact。

## Review

Review `BUILD.bazel` 或 `.bzl` 时至少检查：

1. target 名是否表达真实 owner 与 variant，是否出现 App-specific 公共 library；
2. variant 之间是否真的存在 compile/toolchain/dependency 差异；
3. portable、platform component、launcher、first-party library 与 vendor overlay 的依赖方向，尤其确认 external repository 的 `load` 和全部 target attributes 没有反向引用 first-party label；
4. 最终 artifact 是否只链接一个互相兼容的实现 variant；
5. source ownership、Host Tool/Artifact compatibility、完整 CI 和文档是否随 graph 一起更新；
6. 验证命令是否构建最终平台 target，而不只构建中间 `cc_library`。
7. External rule 是否只发布验证过的 native artifact，并排除 bundle、intermediate、credential、完整 environment 和 flash/monitor/reset 等设备操作；BK7258 AP/CP 与 managed/recovery image 是否仍是不同 provider field。

具体 CI graph、runner grouping 和验证命令继续遵守 [Bazel 构建与 CI 依赖图](/zh/developing/bazel)。
