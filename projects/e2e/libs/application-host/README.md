# Native PAL E2E Application Host

ESP/BK 的 E2E 使用同一个 `h2_pal_e2e_application_host_run(name, capabilities, &config, &result)`。`config` 是现有 `h2_application_host_config_t`；没有第二份 SDK config。`provider` 留空，allocator 默认采用 SDK 的 native allocator。`result` 可留空。基础 PAL 合同与 SDK provider 仍只依赖 `//libs/pal`。

选定 BSP 的 `:application_host` target 链接 `boards/shared/application_host/{esp,bk}` 的完整默认 vtable。prepare 从该 BSP 获取 Runtime config，release 在 Runtime 检查销毁后释放板级 provider；SDK provider 保留启动 task/ISR 校验与 yield。所有入口共用此实现，不在 main 声明 Host vtable。

E2E launcher 在 ESP configure 阶段接入 serial recovery，在 BK validate 阶段接入 serial recovery。测试仍提供必要的 configure/validate 钩子；Core 的独立 transport allocator 由专用钩子先建立，launcher 检查实际 serial owner 状态，避免重复启动。名字和 capabilities 保持各目标的原有协议。普通 App 退出、App 启动失败和初始化失败均先 stop/join command worker 并释放 operation mutex，再销毁 Runtime 和 BSP；清理不能证明成功时保留整个依赖图和 owner task。

各测试保留 watchdog、qualification、confirmation、资源采样、故障注入、恢复与不可变结果回放。报告驻留循环现在响应 Host 的协作 Stop；失败 suite 不能证明所有 fixture 已释放时保留 Runtime 图，Core 的明确 retained cleanup 也继续保留；默认 run_ms=0，仍持续驻留。此 helper 不运行设备、不改变设备 runner 或硬件验收记录。

`esp_*_test` / `bk_*_test` 使用真实共享 Host、Runtime 和 host PAL task/sync，SDK/BSP 接口替身只模拟 native 边界。覆盖成功、无服务、独立 allocator、prepare/configure/Runtime/validate/App 错误、task 启动失败、命令服务停止失败以及 BSP release 失败；检查原始 user context、真实 command worker join、mutex 释放、清理次序与 retained owner 驻留。真实 SDK build 验证实际 native 组件编译/链接/打包；不替代物理板验收。

原始实板/模拟器 qualification 文件保持原始字节和来源。`native_entry_provenance.json` 单独记录六个 Audio/Decoder/Display 入口及其组装文件的前后 SHA，标明新 native qualification 为 NOT_RUN；校验器同时验证当前 assembly SHA 与历史 source SHA。main 的测试/fixture/report token 指纹必须与迁移前相同，数值预算或测试内容变化不能通过入口迁移例外。新增回归拒绝未知 scope、伪称设备 PASS、旧/新 SHA 不匹配以及改短 Audio stability budget。
