# Native PAL E2E Application Host

ESP/BK 的 E2E 使用同一个 `h2_pal_e2e_application_host_run(name, capabilities, &config, &result)`。`config` 是现有 `h2_application_host_config_t`；没有第二份 SDK config。`provider` 留空，allocator 默认采用 SDK 的 native allocator。`result` 可留空。基础 PAL 合同与 SDK provider 仍只依赖 `//libs/pal`。

26 个 ESP/BK PAL E2E 固件入口统一调用 `h2_pal_e2e_application_host_start`，由公共 Host 安装最终 target 的 task policy 并启动 BSP owner task；BK 同时初始化 SDK。异步启动复制应用名和普通 Host config，调用方的栈上 config 可以在 main/app_main 返回后失效；user 和 callback 借用资源仍需覆盖实际运行与 retained 生命周期。每个 main 只声明测试 entry、必要的产品 hook、应用名和 capabilities，不重复 SDK 初始化、BSP task 启动、Host 收尾或 fail/hold 实现。

普通 Host config 的 `before_prepare` 在启动 owner 上先于 BSP prepare 执行。BK Core 用它检查真实双核 Atomic 前提，Backup 用它在 Preferences 初始化前读取原始分区；失败时不执行 BSP prepare/release 或 Runtime 初始化，hook 负责自身资源的安全状态。ESP Core 的栈观测通过目标链接的 task-configure wrapper 接入公共 task-policy 安装，不在 main 另写启动路径。专属 watchdog worker 及不可继续执行的 setup 错误分别调用公共 park/fail；Core 的失败重启与 Backup 的备份错误协议仍由测试拥有。

选定 BSP 的 `:application_host` target 链接 `native_component_src/esp-idf6.x/h2_board_application_host` 或 `native_component_src/bk7258/ap/h2_board_application_host` 的完整默认 vtable。prepare 从该 BSP 获取 Runtime config，release 在 Runtime 检查销毁后释放板级 provider；SDK provider 保留启动 task/ISR 校验与 yield。所有入口共用此实现，不在 main 声明 Host vtable。

E2E launcher 在 ESP configure 阶段接入 serial recovery，在 BK validate 阶段接入 serial recovery。测试仍提供必要的 configure/validate 钩子；Core 的独立 transport allocator 由专用钩子先建立，launcher 检查实际 serial owner 状态，避免重复启动。名字和 capabilities 保持各目标的原有协议。普通 App 退出、App 启动失败和初始化失败均先 stop/join command worker 并释放 operation mutex，再销毁 Runtime 和 BSP；清理不能证明成功时保留整个依赖图和 owner task。

各测试保留 watchdog、qualification、confirmation、资源采样、故障注入、恢复与不可变结果回放。报告驻留循环现在响应 Host 的协作 Stop；失败 suite 不能证明所有 fixture 已释放时保留 Runtime 图，Core 的明确 retained cleanup 也继续保留；默认 run_ms=0，仍持续驻留。此 helper 不运行设备、不改变设备 runner 或硬件验收记录。

App entry 返回错误时，公共 Host 输出失败结果并保持串口恢复服务，等待协作 Stop 后自动保留 Runtime 图；入口无需重复实现 fail/hold 或错误收尾。测试本身的网络等待、case 执行、成功资格确认和不可变报告仍由该测试拥有，不移入 Host。

`esp_*_test` / `bk_*_test` 使用真实共享 Host、Runtime 和 host PAL task/sync，SDK/BSP 接口替身只模拟 native 边界。覆盖成功、无服务、独立 allocator、prepare/configure/Runtime/validate/App 错误、task 启动失败、命令服务停止失败以及 BSP release 失败；检查原始 user context、真实 command worker join、mutex 释放、清理次序与 retained owner 驻留。真实 SDK build 验证实际 native 组件编译/链接/打包；不替代物理板验收。

历史设备记录保留原始字节和来源，不作为当前源码的功能验收。使用 C/C++ 测试直接编译生产实现，在 SDK 边界注入故障并检查运行行为；不抽取源码函数、不以字符串或源码哈希匹配宣称通过。

`esp_serial_shutdown_test`、`esp_command_shutdown_test`、`bk_command_shutdown_test` 直接编译完整生产 C 单元，只替换 SDK 或 transport join 边界，验证拒绝 join 时不释放借用资源、停止接收、mutex 释放失败与重试顺序。

SDK/BSP 公共接线代码由 `native_component_src` 拥有；`boards` 仅声明选定 BSP 的 `:application_host` 组件。`tools/bazel/board_application_host.bzl` 只封装已有 native component 构建声明。
