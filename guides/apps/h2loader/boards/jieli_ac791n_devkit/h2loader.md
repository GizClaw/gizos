# JieLi AC791N DevKit H2Loader <Badge type="warning" text="WIP" />

## 构建

Linux x86_64（或 macOS 上的 Linux x86_64 dev container）：

```sh
bazel build --config=ac791n \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package
```

Managed package 内的 `app/jieli/update.ufw` 是 native updater 消费的 image。空片首刷使用杰理 USB UBOOT 工具或烧写器，之后只通过 H2Loader `send` 加 `reboot upgrade` 更新。

### App watchdog 诊断

`//projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:app_watchdog_package` 是手动诊断 App，不属于任何发布镜像：`:app_watchdog_trial` 带 `manual`，package 带 `no-release` 与 `manual`，`scripts/bazel/bazel-release.py` 的发布目录查询排除 `no-release`，因此它不会进入 release packaging。

该镜像从共享 color-bar launcher 启动；launcher 在确认 trial 前调用强符号 `h2_jieli_target_application_run()` hook，输出一行 `H2_WDT_TRIAL role=app core=<id> control=0x<wdt_con> action=hang`，随后关闭当前核中断并永久执行 `idle`，使该核停止喂狗。它不修改 watchdog 超时或复位模式，也不调用 `wdt_close()`，用于验证双核喂狗策略在任一核停喂时会复位整板。该镜像始终不确认 trial，因此复位回到 P1 后，`jieli_trial_attempt` 仍匹配 P2 App 的 image checksum，PAL 将该 App 判为不可启动，公共 Loader 留在命令模式。

同一 BUILD 文件中的 `:loader_watchdog_package` 在 Loader stage `105` 执行相同的停喂检查。编译通过不代表复位行为已验证；实机记录保留在 PR #178。

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

### Loader 自更新及剩余安全边界

Loader self-update 现在复用 SDK updater，但通过本 layout 的 NOR adapter 暂存 P2 的 32-byte BootInfo，不提前改变 ROM 的启动选择。暂存启动头连同候选 SHA、原生代码长度和 CRC 写入 Preference；候选通过热启动进入 P2，公共 Loader 的确认回调验证候选记录，随后在公共回写流程擦除 P1 之前发布 P2 BootInfo。确认前故障可用 `loader_trial_crash_package` 在 stage 105 注入；应检查旧 P1 恢复、候选保留及 coredump，不能以一次复位代替验收。2026-09-13 已实测不同 Loader 镜像之间的正常路径，以及独立的[确认前故障注入恢复](./evidence/2026-09-13/loader-preconfirm-recovery.md)：候选在 stage 105 保存断言记录并复位，自动返回旧 P1，UART 可查询状态并导出匹配的 coredump。这个测试不覆盖任意硬件异常，也不代表断电验收。

此前通过 GNU `--wrap` 截取 SDK BootInfo 写入的实验已撤销：该工具链的内部 LTO 调用没有经过 wrapper。当前 adapter 完整提供 pinned `update.a` 中 NOR I/O member 的八个导出函数，使 archive 不再抽取原 member，固件反汇编已确认 SDK 调用绑定本实现；实际 NOR 读写、擦除和保护操作仍调用官方驱动。该替换限定于本 NOR layout，不支持 `CONFIG_SDFILE_EXT_ENABLE`。不能重新启用只在主机 mock 中有效、实际固件未拦截的实现。

正常自更新实测的候选包 SHA 为 `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2`：依次观察到 `LOADER_COMMIT mode=warm boot_info=unpublished`、P2 的 native BootInfo 不存在但逻辑分区为 2、`LOADER_TRIAL confirmed=1`、`LOADER_HEADER published=1`、回写事件 2、原生 P1 启动及完成事件 4。最终状态为 P1/next P1、Stage 空、`last_result=0`，运行镜像 SHA 为 `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`。这些证据仅证明正常路径，不证明提交中断恢复。

v2 已完成一次[空闲状态实际断电后的 App 恢复](./evidence/2026-09-13/installed-app-powercycle.md)。最终 v5 也完成了[独立断电复测](./evidence/2026-09-13/installed-app-v5-powercycle.md)：用户断电上电，UART 断开后重新连接，捕获 App 确认与心跳；独立状态查询确认原 App SHA、v5 Loader SHA、Stage 空和 last_result=0。v5 串口重新枚举期间未捕获最早的 Loader 日志，不能宣称记录了全部早期启动过程。提交写入过程中断电仍未验收，本页保持 WIP。

候选启动记录采用固定 112-byte little-endian 编码，不直接持久化 C 结构体填充。字段偏移为 magic 0、代码长度 4、代码 CRC 8、保留字段 10、65-byte SHA 字符串 12、32-byte 启动头 77，尾部 109–111 必须为零。解码同时要求精确长度，后续仍验证候选身份和启动头 CRC。v5 已完成[不同镜像 self-update](./evidence/2026-09-13/loader-v5-self-update.md)、[确认前故障恢复](./evidence/2026-09-13/loader-v5-preconfirm-recovery.md)及下表 UART/BLE 完整回归；不能把这些结果扩展为提交中断或全部 PAL 的验收。

2026-09-13 另做了[无 Stage 的已安装 App 往返验证](./evidence/2026-09-13/installed-app-relaunch.md)：确认 App 后只执行 `reboot loader` 和 `reboot app`，三个状态快照均为 Stage 空，P2 SHA 不变；P1 请求 P2 时没有活动更新或已提交候选，App 重新确认并持续心跳。这项证据独立于安装流程和 E2E 总数，不表示真实断电已经通过。

## BLE

- 控制器配置由 board SDK patch `ble_data_length.patch` 打开 DLE（251-byte LL payload）与 2M PHY。
- ATT MTU 512，ATT send cbuf 为 MTU 的 4 倍。SDK 参考外设固定的 512-byte cbuf 放不下 509-byte notification，会阻塞之后的全部 notification。
- PHY 由 central 选择，PAL 不发起 `LL_PHY_REQ`。CoreBluetooth 自行切换到 2M 后，外设再发起的 PHY 请求会收到 `LL_REJECT_IND`，该控制器在 `ll_slave.c:662` 断言复位。
- Loader 连接后立即请求 15 ms interval。
- Board 不启用 `RF_SLEEP_EN`。开启 RF 睡眠时，控制器会间歇性连续数秒收不到 central 的包并以 supervision timeout 断链，与 PHY、DLE 及 connection update 时机无关；期间两个核的调度间隔最大只有 1–2 个 tick。macOS BLE-only lifecycle 中开启 RF 睡眠 147 次连接断开 9 次，关闭后 72 次连接无断开。

曾经看似需要配置规避的现象都来自两个底层缺陷：compiler-rt `__sync_*` 在双核上丢更新，以及上述 PHY 断言。两者修复后，在同一 DevKit 上逐项复验：1M PHY 下 MTU 512 的多分片 ATT 写入正常；连接后立即请求 15 ms 与 central 的 PHY/DLE 建立交错进行正常；2M PHY 不需要 CSA #2。

macOS 上 854,092-byte App package 的 BLE `send` 端到端 32 s（2M）/ 46 s（1M），2M 稳态约 30 KiB/s。

## E2E runner

macOS 上 BLE Host 由 CoreBluetooth 提供，runner 必须从拥有 Bluetooth 权限的 responsible process（例如 Terminal.app）启动。

```sh
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <serial-endpoint> \
  --ble-id <ble-endpoint> \
  --expected-board jieli_ac791n_devkit \
  --expected-target wl82 \
  --app-firmware <jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib> \
  --loader-firmware <jieli_ac791n_devkit-loader-wl82.update.tar.zlib> \
  --crash-firmware <jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib> \
  --monitor-ms 3000 \
  --report <report.json>
```

Loader 只有 UART 与 BLE capability，不提供 Wi-Fi 与 HTTP；runner 一旦收到 Wi-Fi 参数就会在 Loader 上执行 Wi-Fi case，因此本板不传 URL 与 Wi-Fi 参数。

## 恢复机制与验收边界

历史实机证据见下方[验收记录](#验收记录)。编译与主机测试不能替代实机断电、UART/BLE 生命周期验收。

### 共享 NOR 写保护窗口

固定 SDK 的 suspend/resume 只有一个备份配置字，不可嵌套，也不保证跨任务窗口安全。disk、Pref 和 upgrade adapter 统一通过 board-owned counted window：首个 owner 保存配置并解除保护，最后 owner 恢复，操作错误/短写同样关闭；恢复失败返回 I/O 并禁止后续窗口直到复位。此合同属于板级 NOR 适配，不改变公共 Loader 生命周期。[SDK 反汇编、27-case host/TSan 回归与 Loader/button/PAL 验收](./evidence/2026-09-15/nor-write-protection.md)记录具体覆盖及硬件状态测量限制。

### P2 残缺头的完整重装机制

选择从完整 P1 通过 UART Loader **完整重装 P2**，由本 board layout 的 NOR adapter 与 pinned SDK updater 拥有机制，公共 Loader 不增加板级修复分支。SDK 的 payload 阶段先从 `target_update_addr - 32 = 0x37c000` 擦除 P2 头扇区并由 adapter 完整读回验证，完整 payload 校验后才 arm/capture；因此不会在 arm 时仍面对旧残缺头。擦除错误锁存直到复位，SDK 伪成功完成也不能继续写入或发布，P1 保持可启动。

直接 arm/publish 不修复非空冲突头；有效但不同的完整 bank 也不得自动擦除，只允许显式完整安装替换。相同头 publish 幂等，P2 arm 拒绝，不在 P2 增加恢复擦除。[固定 SDK 追踪、host 回归与两轮 UART 验收](./evidence/2026-09-15/p2-header-reinstall.md)记录擦除命令/地址、镜像 SHA、独立状态及未捕获确认文本的限制。

## 验收记录

### 2026-09-16：main 验收尝试未完成

`667cd925` 的验收在第一步因执行者使用不存在的 button Bazel target 而停止，尚未执行固件安装或硬件套件；这不构成 main 的编译缺陷或硬件失败证据。独立 UART status 确认原 P1 Loader 响应、Stage 空、`last_result=0`。[构建错误、工具 SHA、独立状态与全部未执行项](./evidence/2026-09-16/main-acceptance.md)。

### 2026-09-15：system-event owner 引用计数

Provider 的每次成功 init 各取得一个 owner，Runtime deinit 只释放自己的 owner，launcher/BLE 的订阅继续工作；最后一个 owner 释放后才关闭，并等在途操作退出后销毁。[退出路径的失败回归、TSan 与 button 实机证据](./evidence/2026-09-15/system-event-owners.md)。

### 2026-09-15：共享 launcher 与 Runtime 事件复用

`5b1d822a` 的 ACTIVE event provider 已支持重复初始化；真实 Runtime/provider 主机回归和 button、touch、audio-system 三个生产包实机验证均成功。三个目标通过 UART Loader 安装到 P2，观察 READY、确认成功及独立状态，最后返回 P1。[源码判定、测试与完整验收边界](./evidence/2026-09-15/runtime-event-reuse.md)。

### 2026-09-14：当前源码 Loader 自更新与 UART 回归

源码 `2a814d32`（含 FAT 属性 stat、SDK 单次路径编码和单层 mkdir 修正）在 UID `d879349abc9f` 上完成不同镜像 Loader 自更新：P2 确认并发布启动头后，读取 SD shadow、回写 P1 并收敛；独立 status 确认 P1/P2 SHA 一致、Stage 空、last_result=0。随后在该 Loader 上执行完整 UART suite，25/25 PASS，560.440 秒。两项重连等待明显偏长且原因未定；本轮未覆盖 BLE 或断电。[完整证据与最终板端状态](./evidence/2026-09-14/loader-uart-lifecycle.md)。

### 2026-09-14：当前源码 Loader BLE 连续两轮回归

同一 UID `d879349abc9f`、当前源码 Loader，经重新发现的 BLE endpoint `5:818f070641f0` 完成两轮完整 BLE-only lifecycle：22/22 PASS（385.850 秒）和 22/22 PASS（357.072 秒）。UART 并行记录 34 / 33 次连接，每条连接的 supervision timeout 与 LL reject 均为 0；仍有 SDK `conn nack` 等非致命诊断。独立 UART status 确认 P1 SHA 不变、last_result=0，P2/Stage 保留预期 crash App。未修改代码，不声称已根治历史间歇性问题或覆盖新断电测试。[两轮逐项结果、连接统计与最终状态](./evidence/2026-09-14/loader-ble-lifecycle.md)。

### 2026-09-13：恢复固件后的独立 UART / BLE 回归

在 `3870bf72` 基础上含本文描述的未提交热启动改动，USB DL 使用本次构建输入执行官方 `-format all` 后，设备 UID 为 `3ce9e275d7aa`。报告中的 package SHA-256 是产物身份；不能把未提交构建归为 `3870bf72` 原始产物。

| 路径 | 结果 | 原始报告 |
| --- | --- | --- |
| UART，460800 | 25/25 PASS，283.8 s | [UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-restored-uart-e2e) |
| 首次 BLE | 19/22 PASS，3 FAIL，319.1 s | [首次失败](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-restored-ble-e2e) |
| BLE，UART 旁路记录日志 | 22/22 PASS，339.7 s | [旁路观察](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-ble-full-uart-observed) |
| BLE，无 UART 监控，runner 接入连接失败诊断 | 22/22 PASS，324.4 s | [诊断复测](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-ble-diagnostic-full) |
| BLE，新版延迟发布启动头 Loader（v2） | 22/22 PASS，333.6 s | [新版 BLE](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v2-ble-e2e) |
| UART，新版候选保护，单一读口进程 | 25/25 PASS，284.7 s | [新版 UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-clean-uart-e2e) |
| UART，v5（显式候选记录编码、修正 P1 回写误报） | 25/25 PASS，268.6 s | [v5 UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v5-uart-e2e) |
| BLE，v5，无 UART 并行监控 | 22/22 PASS，342.1 s | [v5 BLE](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v5-ble-e2e) |

这些完整回归包括正常 App 安装与确认、App 命令与传输、跨重启 Stage 保留、正常 Loader self-update、崩溃 App 回滚及 2096-byte coredump 的查询/导出/擦除。测试未提供 Wi-Fi/URL 参数，不覆盖相应能力，也不覆盖实际断电或候选 Loader 崩溃恢复。

首次 BLE 失败发生在 `install-crash-app`，后续 `coredump-status` / `coredump-dump` 因 runner 未取得预期长度而以 0 比较，产生连带失败。首次失败根因尚未确定；后两轮通过不代表该间歇性问题已经修复。runner 新增接线仅转发已有 Host BLE 连接失败诊断，不放宽身份、回滚或 coredump 验收条件。

### 2026-09-13：P1 擦除后的真实断电恢复

[定点断电验收](./evidence/2026-09-13/loader-copy-powercut.md)已通过：候选 P2 确认并发布启动头后，在 P1 启动头擦除完成处暂停，用户实际断电上电。设备继续从 P2 回写 P1，最终两个镜像校验一致、Stage 清空、last_result=0。诊断包使用正式 layout 和 task policy，但含测试停点，不能作为正式发行固件。

随后补齐了以下具体部分写入边界；不能将定点测试扩大为任意损坏模式的保证：

| 中断现场 | 重启后的结果（复位类型见证据） | 证据 |
| --- | --- | --- |
| P2 原生启动头 16-byte 部分写入或 32-byte 错误编程，CRC 无效；P1 完整 | 2026-09-15 当前诊断软件复位后回原 P1；UART 完整重装 button、P2 试运行确认并返回原 P1，两轮均 Stage 清空 | [当前 SDK 完整重装验收](./evidence/2026-09-15/p2-header-reinstall.md) |
| P1 原生启动头仅写入前 16/32 字节；已确认 P2 完整 | P2 恢复回写 P1，最终 P1 启动，Stage 清空 | [P1 部分头](./evidence/2026-09-13/partial-p1-header-powercut.md) |
| Preference 替换期间 Flash 页仅写入前 128/256 字节 | 旧或新值完整，独立 sentinel 保留，重试写入与完整回读成功，P1 Loader 可通信 | [Preference](./evidence/2026-09-13/preference-powercut-plan.md) |
| `lfs_rename` 内部 Flash 页仅写入前 23/256 字节（变化范围 0–45） | 同上；真实 POWER ON 后验证，不混用升级期间旧固件的恢复日志 | [rename 事务](./evidence/2026-09-13/preference-powercut-plan.md#rename-boundary-physical-recovery-pass-for-the-injected-program) |

[历史 P2 部分头测试](./evidence/2026-09-13/loader-partial-p2-header.md)的同身份重装仅证明当时固件的重写、校验与 Stage 清理，不作为当前 deferred-header/O11 实现的验收替代。2026-09-15 的当前源码复测覆盖持久化 CRC 无效的 16/32-byte 头，软件复位验证相同启动现场，不冒充编程中物理断电。未遍历所有 NOR 位损坏组合。

### 2026-09-12：原生更新流程基线

2026-09-12 在 AC791N DevKit 上以 `1bac88f4` 源码构建的三个包完成 UART + BLE 合并回归，46/46 PASS（约 12.4 分钟），BLE 过程中没有 supervision timeout：

- UART 20 项：命令面、payload Stage 与 abort、monitor、`reboot loader/upgrade/app --monitor`、App 安装与确认、App 命令面、两种跨重启 Stage 保留、Loader self-update（P2 候选回写 P1）。
- BLE 20 项：同一生命周期全部经 BLE 执行，包括 App 安装与 Loader self-update。
- crash-before-confirm 回滚后，UART 与 BLE 各自完成 coredump status/dump/erase 与擦除后空白复查。

852 KB App package 的 `send`：BLE 32.6 s，UART 27.6 s。本轮不含 Wi-Fi/URL、真实断电，也不覆盖"Loader 无新 Stage 时启动已安装 App"（见 [JieLi Components](/zh/developing/components/jieli)）。报告与产物身份：[UART + BLE 报告](evidence/2026-09-12/lifecycle-summary.md#retained-acceptance-facts-uart-ble-lifecycle)、[固件包身份](evidence/2026-09-12/lifecycle-summary.md#retained-acceptance-facts-artifacts)。
