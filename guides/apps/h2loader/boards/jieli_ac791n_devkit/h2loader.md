# JieLi AC791N DevKit H2Loader <Badge type="warning" text="WIP" />

本板不在固件发布集合中，canonical `:package` 不标记 `firmware-release`，Release 工作流不提供 AC791N slice。普通 Bazel 构建与 JieLi CI 继续保留。

## 构建

Linux x86_64（或 macOS 上的 Linux x86_64 dev container）：

```sh
bazel build --config=ac791n \
  //projects/h2loader/targets/h2loader_zlib_tar/loader/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_zlib_tar/display/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_zlib_tar/crash-before-confirm/jieli_ac791n_devkit:package
```

Managed package 内的 `app/jieli/update.ufw` 是 native updater 消费的 image。空片首刷使用杰理 USB UBOOT 工具或烧写器，之后只通过 H2Loader `send` 加 `reboot upgrade` 更新。

### App watchdog 诊断

`//projects/h2loader/targets/h2loader_zlib_tar/loader/jieli_ac791n_devkit:app_watchdog_package` 是手动诊断 App，不属于任何发布镜像：`:app_watchdog_trial` 带 `manual`，package 带 `no-release` 与 `manual`，`scripts/bazel/bazel-release.py` 只选择带 `firmware-release` 的 canonical Loader `:package`，并拒绝 alternate package；`no-release` 仅作诊断标记。

该镜像从共享 color-bar launcher 启动；launcher 在确认 trial 前调用强符号 `h2_jieli_target_application_run()` hook，输出一行 `H2_WDT_TRIAL role=app core=<id> control=0x<wdt_con> action=hang`，随后关闭当前核中断并永久执行 `idle`，使该核停止喂狗。它不修改 watchdog 超时或复位模式，也不调用 `wdt_close()`，用于验证双核喂狗策略在任一核停喂时会复位整板。该镜像始终不确认 trial，因此复位回到 P1 后，`jieli_trial_attempt` 仍匹配 P2 App 的 image checksum，PAL 将该 App 判为不可启动，公共 Loader 留在命令模式。

同一 BUILD 文件中的 `:loader_watchdog_package` 在 Loader stage `105` 执行相同的停喂检查。

### Audio stop/restart 诊断

`//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:audio_stop_restart_package` 是手动诊断 App（`manual`、`no-release`，不进入发布目录），用于在实机上证明板级 audio provider 能承受受控的停止→重启循环。它复用 audio-system 场景（Opus 音乐播放加麦克风回环）：入口初始化 Runtime、打印 `H2_JIELI_AUDIO_CYCLE baseline heap_free=<n> tasks=<n> cycles=<N>`，启动 image 自有的 `audio-cycle` worker，并在第一轮 `h2_smoke_audio_system_run()` 返回后以 `H2_JIELI_AUDIO_CYCLE_READY cycles=<N> result=<rc>` 返回该结果，使 launcher 在 120 s trial 窗口内确认；随后 worker 持有 Runtime 完成其余循环。循环次数 N 是 Bazel string build setting，默认 10，可用 `--//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:stop_restart_cycles=50` 覆盖；只接受 [1, 1000] 内不带符号、不带前导零的十进制值，`0`、`010`、`ten` 或 `1001` 等覆盖在 analysis 阶段即失败并指出 flag 与取值，不会进入编译或上板。

每轮循环启动场景、流式运行 3 s、经正常 `h2_smoke_audio_system_stop()` 路径停止（失败时按 audio target 的 100 次、10 ms 间隔有界重试），然后用 `h2_jieli_ac791n_devkit_audio_idle_probe()` 检查 provider 已无打开 track、无保留操作、无 PCM ring 字节、无 SDK server 句柄、麦克风关闭且扬声器停止，并最多等待 2 s 让 `os_tasks_num_query()` 回到参考值。每轮打印一行 `H2_JIELI_AUDIO_CYCLE cycle=<i>/<N> run=<rc> mic_frames=<n> music_frames=<n> consumed=<bytes> stop=<rc> stop_ms=<ms> idle=<0|1> heap_free=<bytes> tasks=<n> result=<ok|fail>`：`run=0`、`mic_frames>0`、`music_frames>0`、`consumed>0`（SDK decoder 确实从 track ring 取走 PCM）、`stop=0`、`idle=1` 且 `tasks` 等于参考值才算 `ok`。参考值取自第 1 轮结束后的堆余量和任务数，因此 SDK 首次打开后按设计常驻的分配不计为泄漏，但 baseline 仍打印以便看到首轮差值。结束时打印 `H2_JIELI_AUDIO_CYCLE_SUMMARY cycles=<N> ok=<n> failed=<n> heap_baseline=<b> heap_ref=<r> heap_last=<l> heap_min=<m> tasks_baseline=<b> tasks_ref=<r> tasks_max=<x> result=<ok|fail>`，`result=ok` 要求全部循环 `ok` 且 `heap_last >= heap_ref`。任一轮 `run()` 或停止失败即终止循环，但汇总行始终打印。

Runtime 归属遵循 audio target 的规则：最后一次停止成功后 worker 调用 `h2_runtime_deinit()`；有界清理耗尽时打印 `cleanup did not complete; Runtime intentionally retained` 并保留 Runtime，不释放 worker 仍可能借用的内存。该镜像在循环结束后保持已确认状态留在 P2，主机用 `reboot loader` 返回 P1。

## 分区与启动

`[0, 0x700000)` 由 SDK double-bank packer 管理。当前 pinned WL82 layout 的两个 SFC 映射基址分别为 `0x4020`、`0x37c020`；它们不是可跨 SDK/layout 复用的公共 PAL 常量。稳定 Loader 在 P1；P2 保存 App，或在 Loader 自更新期间暂存候选 Loader。

App 的安装和再次启动不再依赖擦除 BootInfo：

1. Host 仍发送公共 `tar.zlib` package；Stage、镜像身份和 boot intent 由公共 H2Loader Preference 管理。
2. 安装 App 时 PAL 使用 SDK updater 写入并校验 P2，但不发布 P2 native BootInfo。ROM 仍选择 P1 Loader。
3. PAL 在复位前提交以 P2 image checksum 为值的 trial attempt，并写入 board-layout 专用的单次 RAM handoff。P1 在 SDK `main()` 最早期、CPU1 和应用任务启动前消费 handoff，切换 SFC 映射并进入 P2 `_start`。
4. App 返回 Loader 只需复位；不擦除 App BootInfo 或 App 数据。再次 `reboot app` 不需要新 Stage。App 自重启使用同一 handoff，避免意外执行已经暂存的其它升级包。

热启动组件归属 `boards/jieli_ac791n_devkit/ac791n/layouts/h2loader`，由 Loader target 引入；App 只使用共享 handoff ABI。交接结构的 `sfc.app_addr` 是 CPU 地址，不是 updater `BootInfo.codeLength`。修改 SDK pin、映射、保留 RAM 或链接布局必须重新验证，不能仅凭编译通过沿用上述地址。

持久状态与 RAM handoff 的职责不同：boot intent 和 trial attempt 在 Preference 中持久化；位于 `0x01c7fc00` 的 24-byte handoff 只跨软件复位使用，不承诺跨断电保存。handoff 校验 magic、基址白名单和校验值，并在跳转前清除 magic，拒绝重复消费或不完整请求；无有效请求时继续执行 ROM 选中的 Loader。运行分区身份同时检查实际 SFC 映射与 native BootInfo，候选热启动仅允许有效 handoff 作为补充证据。这里的校验不是固件签名或安全启动机制。

重启调度只在定时回调真正执行复位前发布 handoff，请求的目标随回调参数固定。定时器分配失败返回错误，不提前留下可能被后续无关复位消费的请求。`test_jieli_loader_reboot.py` 执行生产函数的主机桩测试，覆盖分配失败、重复调度、目标固定和普通 Loader 复位；它不代替硬件重启验证。

Loader 在 SD 卡 `/dl` 下保存 image 的原始字节影子（`/dl/.h2loader-image-1`、`/dl/.h2loader-image-2`），用于 Partition 2 校验和 Loader self-update 回写。影子不能放在 `/data`：安装 App 时 image writer 完成后会清空 App 的 data root。

Trial 证据保存在 H2Loader Preference：Loader 在请求 App 启动前写入 `jieli_trial_attempt`（P2 image checksum），App 确认时与 `jieli_trial_checksum`、`jieli_trial_reset_reason` 一起删除。回到 P1 时，若 attempt 仍匹配 P2 App，则 PAL 将该 App 判为不可启动，公共 Loader 留在命令模式；此判断也覆盖没有 Stage 的已安装 App 试运行。不能仅因 Stage 等于 P2 就认定发生回滚。

### Loader 自更新

公共确认、P2→P1 回写和完成条件见 [Loader 更新](../../update/loader)。本 layout 的 NOR adapter 暂存 P2 的 32-byte native BootInfo，候选先通过 RAM handoff 进入 P2；确认前不改变 ROM 的启动选择，失败时回到完整 P1 并保留 Stage、候选及 coredump。`loader_trial_crash_package` 可在 stage 105 注入确认前故障。

候选启动记录保存在 Preference，采用固定 112-byte little-endian 编码：magic 位于 0、代码长度位于 4、代码 CRC 位于 8、保留字段位于 10、65-byte SHA 字符串位于 12、32-byte 启动头位于 77，尾部 109–111 必须为零。解码要求精确长度，并验证候选身份和启动头 CRC；记录缺失、损坏或与候选不匹配时拒绝发布和回写。

P2 的公共确认回调核对运行身份和候选记录；随后 `set_next_boot_partition(2)` 发布 BootInfo，回读验证成功后才允许失效、擦除 P1，并清除 trial attempt。已通过有效 native P2 BootInfo 启动的恢复路径沿用该启动头，无需重新创建热启动记录。复制中断后依靠有效 P2 BootInfo 重启并完整回写。

Adapter 提供 pinned `update.a` 中 NOR I/O member 的八个导出函数，实际 NOR 读写、擦除和保护仍调用官方驱动。该替换限定于本 NOR layout，不支持 `CONFIG_SDFILE_EXT_ENABLE`；SDK 内部 LTO 调用不能通过 GNU `--wrap` 拦截。

## BLE

- 控制器配置由 board SDK patch `ble_data_length.patch` 打开 DLE（251-byte LL payload）与 2M PHY。
- ATT MTU 512，ATT send cbuf 为 MTU 的 4 倍。SDK 参考外设固定的 512-byte cbuf 放不下 509-byte notification，会阻塞之后的全部 notification。
- PHY 由 central 选择，PAL 不发起 `LL_PHY_REQ`。CoreBluetooth 自行切换到 2M 后，外设再发起的 PHY 请求会收到 `LL_REJECT_IND`，该控制器在 `ll_slave.c:662` 断言复位。
- Loader 连接后立即请求 15 ms interval。
- Board 不启用 `RF_SLEEP_EN`，避免控制器因 RF 睡眠长时间收不到 central 的包而发生 supervision timeout。

## E2E runner

macOS 上 BLE Host 由 CoreBluetooth 提供，runner 必须从拥有 Bluetooth 权限的 responsible process（例如 Terminal.app）启动。

```sh
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <serial-endpoint> \
  --ble-id <ble-endpoint> \
  --expected-board jieli_ac791n_devkit \
  --expected-target wl82 \
  --app-firmware <jieli_ac791n_devkit-color-bar-wl82.update.tar> \
  --loader-firmware <jieli_ac791n_devkit-loader-wl82.update.tar> \
  --crash-firmware <jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib> \
  --monitor-ms 3000 \
  --report <report.json>
```

Loader 只有 UART 与 BLE capability，不提供 Wi-Fi 与 HTTP；runner 一旦收到 Wi-Fi 参数就会在 Loader 上执行 Wi-Fi case，因此本板不传 URL 与 Wi-Fi 参数。

## 恢复机制与验收边界

验收分别覆盖正常自更新、确认前故障、P1 回写中断、已安装 App 无 Stage 往返和实际断电恢复。最终停止监控后独立查询 UART status，核对运行 P1、新 Loader image/package metadata、Stage invalid 和 `last_result=0`。SDK pin、布局或启动映射变化后需重新验收。

### 共享 NOR 写保护窗口

固定 SDK 的 suspend/resume 只有一个备份配置字，不可嵌套，也不保证跨任务窗口安全。disk、Pref 和 upgrade adapter 统一通过 board-owned counted window：首个 owner 保存配置并解除保护，最后 owner 恢复，操作错误/短写同样关闭；恢复失败返回 I/O 并禁止后续窗口直到复位。此合同属于板级 NOR 适配，不改变公共 Loader 生命周期。

### P2 残缺头的完整重装机制

选择从完整 P1 通过 UART Loader **完整重装 P2**，由本 board layout 的 NOR adapter 与 pinned SDK updater 拥有机制，公共 Loader 不增加板级修复分支。SDK 的 payload 阶段先从 `target_update_addr - 32 = 0x37c000` 擦除 P2 头扇区并由 adapter 完整读回验证，完整 payload 校验后才 arm/capture；因此不会在 arm 时仍面对旧残缺头。擦除错误锁存直到复位，SDK 伪成功完成也不能继续写入或发布，P1 保持可启动。

直接 arm/publish 不修复非空冲突头；有效但不同的完整 bank 也不得自动擦除，只允许显式完整安装替换。相同头 publish 幂等，P2 arm 拒绝，不在 P2 增加恢复擦除。
