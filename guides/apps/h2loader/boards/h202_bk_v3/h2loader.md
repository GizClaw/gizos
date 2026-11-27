# H202 H2Loader

入口 `projects/h2loader/targets/h2_loader_tar_zlib/loader/haivivi/h202_bk_v3/` 组装公共 H2Loader Core、UART 与 BLE 管理和恢复流程。最小 Runtime 只配置安全的电源电平和外置存储，不启动音频、NFC 或灯效；H202 不使用 4G 模组。[MFG](./mfg.md) 是 Loader 角色，与它互相替换。

## 构建

使用与 GizOS 固定 BK7258 SDK 匹配的工具环境，见 [Bazel BK7258 构建](../../../../../tools/bazel/README.md#bk7258-external-builds)。普通产物由 `:package` 生成：

```bash
bazel build --config=bk7258 \
  //projects/h2loader/targets/h2_loader_tar_zlib/loader/haivivi/h202_bk_v3:package \
  //projects/h2loader/targets/h2_loader_tar_zlib/mfg/haivivi/h202_bk_v3:package
```

Both entries use the standard BK7258 SDK bootloader and the board-owned layout files. Packages retain `board=h202_bk_v3`, `target=bk7258`, and `role=h2loader`. The Loader and MFG use `image=h202-loader` and `image=h202-mfg`, respectively.

## 存储、分区与 SDK 配置

| 项目 | 配置 |
| --- | --- |
| 片内 Flash | 8 MiB；Loader 窗口 2448 KiB，起始 `0x11000`（CP 1156 KiB + AP 1292 KiB） |
| App 窗口 | 5032 KiB，起始 `0x275000`；Loader 与 MFG 都链接在 Loader 窗口 |
| 尾部分区 | `0x75f000` 起依次为 `ota_fina_executive`、`usr_config`、`h2_boot_request`、`flashdb`、`coredump`、`easyflash`、`easyflash_ap`、`sys_rf`、`sys_net`，与此前 H202 布局一致 |
| SD card | Four-bit SDIO on GPIO14–19, card detect on GPIO45. `/dl` and `/data` are directories under `1:/h2loader` on the FAT volume. |
| Preference | FlashDB KVDB，起始 `0x780000`，大小 `0x6000` |
| Console | UART1（GPIO0/1），460800 baud；ROM 下载走 UART0（GPIO10/11） |

权威输入是 `boards/haivivi/h202_bk_v3/bk7258/ap.defaults`、`cp.defaults`、`layouts/{loader,mfg}/` 下的 defaults，以及共用的 `layouts/loader/partitions/bk7258/auto_partitions.csv`；`layouts/mfg` 关闭蓝牙并保留 MFG 的 RAM 划分。App 窗口不小于 Loader 窗口，供 Loader 角色候选中转。

Standard Loader retries SD mounting while keeping UART diagnostics available. MFG can format a card during its initial SD check. GPIO45 is an active-low input with internal pull resistors disabled. See [SD card test](./mfg.md#sd-card-test).

## Installation and recovery

Use the `.update.tar.zlib` file from `:package` with Gizloader. Connect to the management UART of the running H202 Loader or MFG. The CLI uses the same protocol.

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 status
```

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 send --file /absolute/path/to/h202_bk_v3-h202-mfg-bk7258.update.tar.zlib
```

发送后先核对 Stage 的版本、role、board、target 和包/镜像摘要，再触发升级：

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 reboot upgrade --monitor
```

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 status
```

验收必须观察 P2 候选执行、确认和回写，最终 P1 的版本与 SHA 匹配、`stage_valid=0`、`last_result=0`。Loader→MFG→Loader 两个方向分别记录；只发送成功、预期拒绝或 ROM 恢复成功都不能替代管理安装验收。未完成的 MFG 仍可接收 Loader 角色修复，普通 App 的厂测门禁继续生效。

A managed Loader update uses the App window for its candidate and overwrites the prior executable App. Reinstall the App if needed. Managed updates leave the bootloader in place. Use the board-matched recovery image from the same build and the ROM UART if the management interface is unavailable.

Record hardware qualification for the exact commit and package in both directions.
