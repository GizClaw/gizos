# H202 H2Loader

入口 `projects/h2loader/targets/h2loader_tar_zlib/loader/h202_bk_v2/` 组装公共 H2Loader Core、UART 与 BLE 管理和恢复流程。最小 Runtime 只配置安全的电源电平和外置存储，不启动音频、NFC 或灯效；H202 不使用 4G 模组。[MFG](./mfg) 是 Loader 角色，与它互相替换。

## 构建

使用与 GizOS 固定 BK7258 SDK 匹配的工具环境，见 [Bazel BK7258 构建](../../../../../tools/bazel/README.md#bk7258-external-builds)。普通产物由 `:package` 生成：

```bash
bazel build --config=bk7258 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/h202_bk_v2:package \
  //projects/mfg/apps/h202_bk_v2:package
```

两个入口都通过显式 `layout_files` 选择 `boards/h202_bk_v2/bk7258/layouts/`，并以 `bootloader_patch` 选择 `//third_party/bk7258_bootloader_patch:mapped_ab_v1`。包 identity 为 `board=h202_bk_v2`、`target=bk7258`、`role=h2loader`；Loader 为 `image=h202-loader`，MFG 为 `image=h202-mfg`。构建记录 `partition-metadata/bootloader.json` 中的 bootloader 合同和身份，SDK 安装目录保持不变。

## 存储、分区与 SDK 配置

| 项目 | 配置 |
| --- | --- |
| 片内 Flash | 8 MiB；Loader 窗口 2448 KiB，起始 `0x11000`（CP 1156 KiB + AP 1292 KiB） |
| App 窗口 | 5032 KiB，起始 `0x275000`；Loader 与 MFG 都链接在 Loader 窗口 |
| 尾部分区 | `0x75f000` 起依次为 `ota_fina_executive`、`usr_config`、`h2_boot_request`、`flashdb`、`coredump`、`easyflash`、`easyflash_ap`、`sys_rf`、`sys_net`，与此前 H202 布局一致 |
| 外置 Flash | QSPI0（GPIO22-27），LittleFS 固定 8 MiB；`/dl` 与 `/data` 是同一卷下 `h2loader/` 的两个目录 |
| Preference | FlashDB KVDB，起始 `0x780000`，大小 `0x6000` |
| Console | UART1（GPIO0/1），460800 baud；ROM 下载走 UART0（GPIO10/11） |

权威输入是 `boards/h202_bk_v2/bk7258/ap.defaults`、`cp.defaults`、`layouts/{loader,mfg}/` 下的 defaults，以及共用的 `layouts/loader/partitions/bk7258/auto_partitions.csv`；`layouts/mfg` 关闭蓝牙并保留 MFG 的 RAM 划分。App 窗口不小于 Loader 窗口，供 Loader 角色候选中转。

外置 Flash 和 NFC、RGB 都由 GPIO54 控制的 `WAKE_3V3` 供电。板级在挂载存储前打开它；标准 Loader 挂载失败时保留诊断并重试，不自动格式化。MFG 启动的首次挂载失败可格式化并重挂载一次，见 [MFG 外置 Flash 检查](./mfg#外置-spi-flash-启动检查)。该板支持 QSPI Flash，不使用 SD NAND。早期设备的外置 Flash JEDEC 为 `0x856017`，卷容量固定为 8 MiB，实际 JEDEC 由启动日志确认。

## MP_C 与 MP_J

MP_C（AES-off）使用 `:package` 的普通包。MP_J（AES-on）使用统一规则 `h2loader_encrypted_package(name = "encrypted_package", source = ":package")` 的加密产物：

```bash
bazel build --config=bk7258 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/h202_bk_v2:encrypted_package \
  //projects/mfg/apps/h202_bk_v2:encrypted_package \
  --inject_repository=private_keys=/absolute/path/to/private-keys \
  --//projects/h2loader/tools/bazel:encryption_key=@private_keys//:device.key \
  --output_groups=+encryption_evidence
```

`/absolute/path/to/private-keys` 是仓库外的私有目录，含空的 `REPO.bazel`、导出 `device.key` 的 `BUILD.bazel` 和设备匹配的 32 字节密钥文件。key 是加密 action 的声明输入；native 编译不接触 key，Gizloader 和设备也不接收 key。缺失或长度错误会失败，不回退到明文。加密 action 为 local、no-remote、no-cache；Bazel 仍保留本地输入摘要和密文输出。普通 CI 使用公共测试 key，不使用设备私钥。

加密目标继承普通目标的身份、版本、格式和数据，输出目录为 `encrypted_package/`。Gizloader 使用其中的标准 `.update.tar.zlib`；`encryption_evidence` 还提供密文 `.flash-aes.rbl`、完整恢复 `.flash-aes.bin` 和回执。旧的 `:encrypted_recovery`、运行时 `--key-fd` 和私有离线加密脚本不再是 H202 构建入口。

## 安装与恢复

AES-on 的 Loader/MFG 管理升级需要同时具备：

1. 板内已安装与布局匹配的 `mapped_ab_v1` bootloader。
2. 当前运行的 Loader/MFG 已包含 [PR832](https://github.com/GizClaw/gizos/pull/832) 的通用 BK adapter 支持。
3. 包已用匹配设备的 key 构建，board、target、role、版本和摘要正确。

两个 H202 入口均在初始化时绑定 `h2_bk_h2loader_boot_context_bind(pref, mem)`，绑定失败不继续开放管理。绑定的 API 和状态保持存活；没有 Runtime 销毁路径，管理重启由整机复位结束。

满足基线后，Gizloader 在 Loader 管理口（当前实验板为 110 口）发送标准加密包，再执行 `reboot upgrade`。CLI 使用同一协议：

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 status
```

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 send --file /absolute/path/to/encrypted_package/h202_bk_v2-h202-mfg-bk7258.update.tar.zlib
```

发送后先核对 Stage 的版本、role、board、target 和包/镜像摘要，再触发升级：

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 reboot upgrade --monitor
```

```bash
h2loader --no-ble --port /dev/cu.usbserial-110 status
```

验收必须观察 P2 候选执行、确认和回写，最终 P1 的版本与 SHA 匹配、`stage_valid=0`、`last_result=0`。Loader→MFG→Loader 两个方向分别记录；只发送成功、预期拒绝或 ROM 恢复成功都不能替代管理安装验收。未完成的 MFG 仍可接收 Loader 角色修复，普通 App 的厂测门禁继续生效。

Loader 自升级借用 App 窗口，会覆盖其中原 App 的可执行镜像；需要 App 时应重新安装。管理升级包不包含 bootloader，也不会修改其区域。原厂 bootloader 或不具备兼容性的设备，先用匹配的加密 recovery 建立基线；ROM 还用于无法通信设备的恢复。完整恢复只覆盖 bootloader 与 Loader 窗口，尾部数据分区不在其范围内。

本接入依赖 [Issue828](https://github.com/GizClaw/gizos/issues/828) 的通用合同。构建成功不代表最终真机验收通过，设备基线和串口使用应在硬件测试前单独协调。
