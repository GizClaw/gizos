# DevKit Libco Smoke

`projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit` 把 `projects/e2e/apps/libco` 持有的 portable `libco-smoke` 编译成 board `devkit`、target `esp32s3`、role `app`、app name `libco-smoke` 的 H2Loader package。

## Execution Boundary

Executor root 是通过现有 PAL Task API 创建的 `libco/entry`，最终 target 的 task policy 将它固定到 core 0，使用 64 KiB PSRAM stack。SDK `app_main` 只安装 task policy、启动入口并返回；Runtime 初始化、portable scenario、H2Loader 确认和 hold 都在同一个入口 task 内完成。所有 coroutine 都在这一个 outer task 内 cooperative switch，每个 coroutine 仍拥有显式 8 KiB stack；SDK main stack 配置不变。GizOS-owned backend 只支持锁定 ESP-IDF 6.x 的 ESP32-S3 windowed ABI；它不是通用 Xtensa、P4 或 C5 backend。

启动输出 `H2_ESP_LIBCO_ROOT`，包含实际 outer task 名称和 core、8 KiB coroutine stack 与 10,000 switch 配置。Launcher 只把 `libco-smoke` scope 的 INFO 测试报告直接交付 stdout，其他日志继续使用原 board Log provider；不修改 SDK 或日志等级。Portable scenario 完成全部 `H2_LIBCO_SMOKE_STAGE` 后只输出一次 `H2_LIBCO_SMOKE_PASS rc=0`；ROOT 或 portable 报告的输出错误沿原失败路径阻止确认。App 随后以自身 identity 完成 Partition 2 metadata 与 Stage 收尾，并输出一次 `H2_ESP_LIBCO_SMOKE_READY rc=0`；确认或 READY 输出失败均进入 FAIL/hold，READY 输出错误不会撤销已经完成的确认，也不能取得测试资格。

## Build And Physical Acceptance

```sh
bazel build --config=esp32s3  //projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit:package
```

必须按 [CLI 使用说明](/zh/using/h2loader/cli) 通过 H2Loader managed flow 安装，验证 active identity 与 package manifest 一致、运行 Partition 2、Partition 2 metadata valid 且 Stage invalid。串口证据必须包含完整 portable stage、PASS 和 READY，且没有 FAIL、watchdog、window exception、reset loop 或 coredump。随后必须通过 fixture 的外部硬件复位线或真实断电再上电触发 fresh boot，按结构化 identity 重新发现设备并重复完整 scenario、status 与 coredump 检查；App 内的软件 reboot 不能代替这个硬件启动边界。
