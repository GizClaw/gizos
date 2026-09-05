# BK7258 V3 Libco Smoke

`projects/e2e/targets/h2loader_tar_zlib/libco-smoke/bk7258_v3_202405` 把 `projects/e2e/apps/libco` 持有的 portable `libco-smoke` 编译成 board `bk7258_v3_202405`、target `bk7258`、role `app`、app name `libco-smoke` 的 H2Loader package。

## Execution Boundary

Executor 只在 Board-owned AP entry task 内创建和调度。Bazel 使用 BK7258 Cortex-M33 target toolchain 把 `//libs/pal/providers/libco:libco` 编译成唯一 `.a`，AP component 只通过 imported target 消费这个 archive，不再编译第二份 libco source。Backend 按 AAPCS 保存 callee-saved core 和可用的 floating-point registers；Armv8-M 的 `PSPLIM` 与 `PRIMASK` 也属于每个 coroutine context，切换时必须在关中断窗口内同步 stack limit 与 SP。每个 coroutine 显式拥有 8 KiB stack，backend context control words 从该空间中保留，最多三个 coroutine 同时存活。Image 保留 board-standard AP/CP Bluetooth controller 与 IPC 初始化，避免拆开双核启动契约；libco 验收和 UART/iKCP App command service 不依赖 BLE advertising，CP 也不链接或调用 libco。Coroutine handle 不能跨 AP task、IRQ、callback 或 CP mailbox。

启动输出 `H2_BK_AP_BOOT image=libco-smoke`，包含 8 KiB stack 和 10,000 switch 配置。Portable scenario 完成全部 `H2_LIBCO_SMOKE_STAGE` 后只输出一次 `H2_LIBCO_SMOKE_PASS rc=0`；App 随后以自身 identity 完成 Partition 2 metadata 与 Stage 收尾，并输出一次 `H2_BK_LIBCO_SMOKE_READY rc=0`。任一失败输出 `H2_BK_LIBCO_SMOKE_FAIL`，不会提交 image metadata。

## Build And Physical Acceptance

```sh
bazel build --config=bk7258  //projects/e2e/targets/h2loader_tar_zlib/libco-smoke/bk7258_v3_202405:package
```

必须按 [CLI 使用说明](/zh/using/h2loader/cli) 通过 H2Loader managed flow 安装，验证 active identity 与 package manifest 一致、运行 Partition 2、Partition 2 metadata valid 且 Stage invalid。串口证据必须包含完整 portable stage、PASS 和 READY，且没有 FAIL、watchdog、reset loop、guard fault 或 coredump。随后必须通过 fixture 的外部硬件复位线或真实断电再上电触发 fresh boot，按结构化 identity 重新发现设备并重复完整 scenario、status 与 coredump 检查；App 内的软件 reboot 不能代替这个硬件启动边界。
