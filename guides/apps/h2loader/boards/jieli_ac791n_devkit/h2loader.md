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

Loader self-update 现在复用 SDK updater，但通过本 layout 的 NOR adapter 暂存 P2 的 32-byte BootInfo，不提前改变 ROM 的启动选择。暂存启动头连同候选 SHA、原生代码长度和 CRC 写入 Preference；候选通过热启动进入 P2，公共 Loader 的确认回调验证候选记录，随后在公共回写流程擦除 P1 之前发布 P2 BootInfo。2026-09-13 已实测不同 Loader 镜像之间的正常路径，以及独立的[确认前故障注入恢复](./evidence/2026-09-13/loader-preconfirm-recovery.md)：候选在 stage 105 保存断言记录并复位，自动返回旧 P1，UART 可查询状态并导出匹配的 coredump。这个测试不覆盖任意硬件异常，也不代表断电验收。

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

## 验收记录

### 2026-09-13：恢复固件后的独立 UART / BLE 回归

在 `3870bf72` 基础上含本文描述的未提交热启动改动，USB DL 使用本次构建输入执行官方 `-format all` 后，设备 UID 为 `3ce9e275d7aa`。报告中的 package SHA-256 是产物身份；不能把未提交构建归为 `3870bf72` 原始产物。

| 路径 | 结果 | 原始报告 |
| --- | --- | --- |
| UART，460800 | 25/25 PASS，283.8 s | [UART](./evidence/2026-09-13/restored-uart-e2e.json) |
| 首次 BLE | 19/22 PASS，3 FAIL，319.1 s | [首次失败](./evidence/2026-09-13/restored-ble-e2e.json) |
| BLE，UART 旁路记录日志 | 22/22 PASS，339.7 s | [旁路观察](./evidence/2026-09-13/ble-full-uart-observed.json) |
| BLE，无 UART 监控，runner 接入连接失败诊断 | 22/22 PASS，324.4 s | [诊断复测](./evidence/2026-09-13/ble-diagnostic-full.json) |
| BLE，新版延迟发布启动头 Loader（v2） | 22/22 PASS，333.6 s | [新版 BLE](./evidence/2026-09-13/deferred-v2-ble-e2e.json) |
| UART，新版候选保护，单一读口进程 | 25/25 PASS，284.7 s | [新版 UART](./evidence/2026-09-13/deferred-clean-uart-e2e.json) |
| UART，v5（显式候选记录编码、修正 P1 回写误报） | 25/25 PASS，268.6 s | [v5 UART](./evidence/2026-09-13/deferred-v5-uart-e2e.json) |
| BLE，v5，无 UART 并行监控 | 22/22 PASS，342.1 s | [v5 BLE](./evidence/2026-09-13/deferred-v5-ble-e2e.json) |

这些完整回归包括正常 App 安装与确认、App 命令与传输、跨重启 Stage 保留、正常 Loader self-update、崩溃 App 回滚及 2096-byte coredump 的查询/导出/擦除。测试未提供 Wi-Fi/URL 参数，不覆盖相应能力，也不覆盖实际断电或候选 Loader 崩溃恢复。

首次 BLE 失败发生在 `install-crash-app`，后续 `coredump-status` / `coredump-dump` 因 runner 未取得预期长度而以 0 比较，产生连带失败。首次失败根因尚未确定；后两轮通过不代表该间歇性问题已经修复。runner 新增接线仅转发已有 Host BLE 连接失败诊断，不放宽身份、回滚或 coredump 验收条件。

### 2026-09-12：原生更新流程基线

2026-09-12 在 AC791N DevKit 上以 `1bac88f4` 源码构建的三个包完成 UART + BLE 合并回归，46/46 PASS（约 12.4 分钟），BLE 过程中没有 supervision timeout：

- UART 20 项：命令面、payload Stage 与 abort、monitor、`reboot loader/upgrade/app --monitor`、App 安装与确认、App 命令面、两种跨重启 Stage 保留、Loader self-update（P2 候选回写 P1）。
- BLE 20 项：同一生命周期全部经 BLE 执行，包括 App 安装与 Loader self-update。
- crash-before-confirm 回滚后，UART 与 BLE 各自完成 coredump status/dump/erase 与擦除后空白复查。

852 KB App package 的 `send`：BLE 32.6 s，UART 27.6 s。本轮不含 Wi-Fi/URL、真实断电，也不覆盖"Loader 无新 Stage 时启动已安装 App"（见 [JieLi Components](/zh/developing/components/jieli)）。报告与产物身份：[UART + BLE 报告](./evidence/2026-09-12/uart-ble-lifecycle.json)、[固件包身份](./evidence/2026-09-12/artifacts.json)。
