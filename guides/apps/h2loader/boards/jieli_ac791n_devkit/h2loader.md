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

`[0, 0x700000)` 由 SDK double-bank packer 管理，Loader/App 是逻辑角色而不是固定地址分区。杰理没有非破坏性选择已编程 bank 的接口：跨 bank 启动只允许紧接在本次提交的新 bank 之后。App 返回 Loader 时清除自身 BootInfo。

Loader 在 SD 卡 `/dl` 下保存 image 的原始字节影子（`/dl/.h2loader-image-1`、`/dl/.h2loader-image-2`），用于 Partition 2 校验和 Loader self-update 回写。影子不能放在 `/data`：安装 App 时 image writer 完成后会清空 App 的 data root。

Trial 证据保存在 H2Loader Preference：Loader 在烧写 App BootInfo 之前写入 `jieli_trial_attempt`（Partition 2 image checksum），App 确认时与 `jieli_trial_checksum`、`jieli_trial_reset_reason` 一起删除。Loader 启动时只有 Stage 等于 Partition 2 且 attempt 仍在，才判定为回滚；重新 stage 同一个已安装 package 仍可安装。

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

2026-09-12 在 AC791N DevKit 上以 `1bac88f4` 源码构建的三个包完成 UART + BLE 合并回归，46/46 PASS（约 12.4 分钟），BLE 过程中没有 supervision timeout：

- UART 20 项：命令面、payload Stage 与 abort、monitor、`reboot loader/upgrade/app --monitor`、App 安装与确认、App 命令面、两种跨重启 Stage 保留、Loader self-update（P2 候选回写 P1）。
- BLE 20 项：同一生命周期全部经 BLE 执行，包括 App 安装与 Loader self-update。
- crash-before-confirm 回滚后，UART 与 BLE 各自完成 coredump status/dump/erase 与擦除后空白复查。

852 KB App package 的 `send`：BLE 32.6 s，UART 27.6 s。本轮不含 Wi-Fi/URL、真实断电，也不覆盖"Loader 无新 Stage 时启动已安装 App"（见 [JieLi Components](/zh/developing/components/jieli)）。报告与产物身份：[UART + BLE 报告](./evidence/2026-09-12/uart-ble-lifecycle.json)、[固件包身份](./evidence/2026-09-12/artifacts.json)。
