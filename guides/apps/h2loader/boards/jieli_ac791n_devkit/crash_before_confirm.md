# JieLi AC791N DevKit Crash Before Confirm

`crash-before-confirm` 复用 color-bar launcher 的 trial boot、UART/BLE App command service 和确认流程，在两者之间通过 SDK `cpu_assert_debug()` 触发断言。它用于验证未确认 App 的 trial rollback、Loader 不修改 Stage 与 Partition 2 metadata，以及 UART/BLE coredump 读取；一次断言或重启本身不算通过。

## 构建

```sh
bazel build --config=ac791n \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package
```

## 预期表现

1. Loader 在提交 App bank 之前写入 `jieli_trial_attempt`（Partition 2 image checksum）。
2. App 第一次启动记录 `jieli_trial_checksum` 后断言；断言 hook 把崩溃记录写入 retained RAM，并标记来源为 App。
3. App bank 仍被选中，第二次启动识别为重复 trial，清除自身 BootInfo 并软复位回到 Loader。
4. Loader 看到 Stage 等于 Partition 2 且 attempt 仍存在，报告 Partition 2 不可启动并留在 command mode；Stage、Partition 2 metadata、`boot_intent=auto` 与 `last_result` 保持不变。
5. Loader 把 retained 记录写入 coredump 分区。崩溃来源是 App，因此 Loader 不进入降级恢复，BLE command service 照常启动，UART 和 BLE 都能读取与擦除 coredump。
