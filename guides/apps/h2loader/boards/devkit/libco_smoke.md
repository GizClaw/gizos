# DevKit Libco Smoke

`projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit` 把 `projects/e2e/apps/libco` 持有的 portable `libco-smoke` 编译成 board `devkit`、target `esp32s3`、role `app`、app name `libco-smoke` 的 H2Loader package。

## Execution Boundary

Executor root 是 ESP-IDF 已有且固定到 `ESP_TASK_MAIN_CORE` 的 `main_task`。Launcher 不为 App 再创建 FreeRTOS task；所有 coroutine 都在这一个 outer task 内 cooperative switch，但每个 coroutine 仍拥有显式 8 KiB stack。GizOS-owned backend 只支持锁定 ESP-IDF 6.x 的 ESP32-S3 windowed ABI；它不是通用 Xtensa、P4 或 C5 backend。

启动输出 `H2_ESP_LIBCO_ROOT`，包含 outer task core、8 KiB stack 和 10,000 switch 配置。Portable scenario 完成全部 `H2_LIBCO_SMOKE_STAGE` 后只输出一次 `H2_LIBCO_SMOKE_PASS rc=0`；trial image 随后确认并输出一次 `H2_ESP_LIBCO_SMOKE_READY rc=0`。任一失败输出 `H2_ESP_LIBCO_SMOKE_FAIL`，不会确认 image。

## Build And Physical Acceptance

```sh
bazel build --config=esp32s3  //projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit:package
```

必须按 [CLI 使用说明](/zh/using/h2loader/cli) 通过 H2Loader managed flow 安装，验证 `active_role=app`、`active_name=libco-smoke`、`state=confirmed`、matching checksum 和空 staged state。串口证据必须包含完整 portable stage、PASS 和 READY，且没有 FAIL、watchdog、window exception、reset loop 或 coredump。随后必须通过 fixture 的外部硬件复位线或真实断电再上电触发 fresh boot，按结构化 identity 重新发现设备并重复完整 scenario、status 与 coredump 检查；App 内的软件 reboot 不能代替这个硬件启动边界。
