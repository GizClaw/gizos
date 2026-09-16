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

Loader self-update 现在复用 SDK updater，但通过本 layout 的 NOR adapter 暂存 P2 的 32-byte BootInfo，不提前改变 ROM 的启动选择。暂存启动头连同候选 SHA、原生代码长度和 CRC 写入 Preference；候选通过热启动进入 P2，公共 Loader 的确认回调验证候选记录，随后在公共回写流程擦除 P1 之前发布 P2 BootInfo。确认前故障可用 `loader_trial_crash_package` 在 stage 105 注入；应检查旧 P1 恢复、候选保留及 coredump，不能以一次复位代替验收。

此前通过 GNU `--wrap` 截取 SDK BootInfo 写入的实验已撤销：该工具链的内部 LTO 调用没有经过 wrapper。当前 adapter 完整提供 pinned `update.a` 中 NOR I/O member 的八个导出函数，使 archive 不再抽取原 member，固件反汇编已确认 SDK 调用绑定本实现；实际 NOR 读写、擦除和保护操作仍调用官方驱动。该替换限定于本 NOR layout，不支持 `CONFIG_SDFILE_EXT_ENABLE`。不能重新启用只在主机 mock 中有效、实际固件未拦截的实现。

候选启动记录采用固定 112-byte little-endian 编码，不直接持久化 C 结构体填充。字段偏移为 magic 0、代码长度 4、代码 CRC 8、保留字段 10、65-byte SHA 字符串 12、32-byte 启动头 77，尾部 109–111 必须为零。解码同时要求精确长度，后续仍验证候选身份和启动头 CRC。

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

本页只描述 Loader 镜像及诊断入口；历史实机记录保留在 PR #178，不随本次拆分引入。编译与主机测试不能替代实机断电、UART/BLE 生命周期验收。

### 共享 NOR 写保护窗口

固定 SDK 的 suspend/resume 只有一个备份配置字，不可嵌套，也不保证跨任务窗口安全。disk、Pref 和 upgrade adapter 统一通过 board-owned counted window：首个 owner 保存配置并解除保护，最后 owner 恢复，操作错误/短写同样关闭；恢复失败返回 I/O 并禁止后续窗口直到复位。此合同属于板级 NOR 适配，不改变公共 Loader 生命周期。

### P2 残缺头的完整重装机制

选择从完整 P1 通过 UART Loader **完整重装 P2**，由本 board layout 的 NOR adapter 与 pinned SDK updater 拥有机制，公共 Loader 不增加板级修复分支。SDK 的 payload 阶段先从 `target_update_addr - 32 = 0x37c000` 擦除 P2 头扇区并由 adapter 完整读回验证，完整 payload 校验后才 arm/capture；因此不会在 arm 时仍面对旧残缺头。擦除错误锁存直到复位，SDK 伪成功完成也不能继续写入或发布，P1 保持可启动。

直接 arm/publish 不修复非空冲突头；有效但不同的完整 bank 也不得自动擦除，只允许显式完整安装替换。相同头 publish 幂等，P2 arm 拒绝，不在 P2 增加恢复擦除。
