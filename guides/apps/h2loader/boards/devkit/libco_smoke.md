# DevKit Libco Smoke

`projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit` 把 `projects/e2e/apps/libco` 持有的 portable `libco-smoke` 编译成 board `devkit`、target `esp32s3`、role `app`、app name `libco-smoke` 的 H2Loader package。

## Execution Boundary

Executor root 是 ESP-IDF 已有且固定到 `ESP_TASK_MAIN_CORE` 的 `main_task`。Launcher 不为 App 再创建 FreeRTOS task；所有 coroutine 都在这一个 outer task 内 cooperative switch，但每个 coroutine 仍拥有显式 8 KiB stack。GizOS-owned backend 只支持锁定 ESP-IDF 6.x 的 ESP32-S3 windowed ABI；它不是通用 Xtensa、P4 或 C5 backend。

Portable scenario 只借用 Memory、Time 和 Log。Launcher 与独立 PAL Core/Crypto/JSON/Storage 套件一样，把本次 Runtime 的 `system_event` 绑定为 canonical unsupported，避免启动无关的 netif/modem 事件观察。该选择只属于 Libco 入口，不改变 BSP 或 Runtime provider。恢复管理只启动 UART command service，发布 `H2_LOADER_CAPABILITY_UART`，通过 iostreamikcp 操作；此入口不启动 BLE command service。

Suite defaults 显式保留 DevKit 既有的 64 KiB `main_task` stack，供 Runtime 初始化与 H2Loader metadata/Preference 确认使用；consumer Board 复用同一份 defaults。该预算不改变每个 coroutine 的 8 KiB stack 或 10,000 次 switch。INFO 日志和 DEBUG 编译上限确保 portable stage/PASS 真实到达 UART。

`H2_ESP_LIBCO_CHECKPOINT` 在 boot、Runtime 初始化后、portable scenario 前后和 confirmation 前后报告 SDK heap integrity、当前 main stack 余量与历史最小余量，单位均为 bytes；`rc` 是这些检查的真实返回结果，输出还须通过 `printf` 与 `fflush` 的结果检查。启动检查失败在 command transport 可用后保持 FAIL，后续检查失败直接保留管理服务。Confirmation 前的失败阻止确认；confirmation 后的失败阻止 READY，并按实际 metadata 记录已经完成的提交。Checkpoint 不替代 portable ledger 或独立启动证据。

确认前从已启动的 command service 取得 borrowed `operation_sync`/`operation_mutex`，在同一 root task 上依次 lock、调用 `h2_esp_h2loader_app_confirm()`、unlock，与串口 Stage、digest、status 和 reboot 操作共用串行化边界。取得配置或锁失败时不调用确认；确认失败也必须释放锁并保留首个错误。`H2_ESP_LIBCO_CONFIRM rc=<result>` 在确认包装返回后立即输出，随后才运行 `after_confirm` 检查点；错误保持 FAIL，不输出 READY。

启动输出 `H2_ESP_LIBCO_ROOT`，包含 outer task core、8 KiB coroutine stack 和 10,000 switch 配置。Portable scenario 完成全部 `H2_LIBCO_SMOKE_STAGE` 后只输出一次 `H2_LIBCO_SMOKE_PASS rc=0`；App 随后以自身 identity 完成 Partition 2 metadata 与 Stage 收尾，并在所有检查通过后输出一次 `H2_ESP_LIBCO_SMOKE_READY rc=0`。任一失败输出 `H2_ESP_LIBCO_SMOKE_FAIL`，恢复时以实际 `status` 中的 metadata 为准。

## Build And Physical Acceptance

```sh
bazel build --config=esp32s3  //projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit:package
```

必须按 [CLI 使用说明](/zh/using/h2loader/cli) 通过 H2Loader managed flow 安装，验证 active identity 与 package manifest 一致、运行 Partition 2、Partition 2 metadata valid 且 Stage invalid。串口证据必须包含完整 portable stage、PASS 和 READY，且没有 FAIL、watchdog、window exception、reset loop 或 coredump。随后必须通过 fixture 的外部硬件复位线或真实断电再上电触发 fresh boot，按结构化 identity 重新发现设备并重复完整 scenario、status 与 coredump 检查；App 内的软件 reboot 不能代替这个硬件启动边界。
