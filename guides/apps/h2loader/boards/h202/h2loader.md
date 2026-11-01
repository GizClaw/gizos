# H202 H2Loader

入口 `projects/h2loader/targets/h2loader_tar_zlib/loader/h202/` 组装公共 H2Loader Core、UART 与 BLE 管理和恢复流程。最小 Runtime 只配置安全的电源电平和外置存储，不启动音频、NFC、4G 或灯效。MFG 是由它安装的 [App](./mfg)。

## 构建

使用与 gizos 固定 BK7258 SDK 匹配的工具环境：

```sh
bazel build --config=bk7258 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/h202:package \
  //projects/mfg/apps/h202:package
bazel build //projects/h2loader/targets/cc_binary/cli:h2loader
```

两个入口都通过显式 `layout_files` 选择 `boards/h202/bk7258/layouts/`，不修改 `h2loader_firmware.bzl` 的内建板表。包 identity 为 `board=h202`、`target=bk7258`；Loader 为 `image=h202-loader`、`role=h2loader`。

## 存储、分区与 SDK 配置

| 项目 | 配置 |
| --- | --- |
| 片内 Flash | 8 MiB；Loader 窗口 2448 KiB，起始 `0x11000`（CP 1156 KiB + AP 1292 KiB） |
| App 窗口 | 5032 KiB，起始 `0x275000`；MFG 的 CP 1156 KiB、AP 3876 KiB 按此地址链接 |
| 尾部分区 | `0x75f000` 起依次为 `ota_fina_executive`、`usr_config`、`h2_boot_request`、`flashdb`、`coredump`、`easyflash`、`easyflash_ap`、`sys_rf`、`sys_net`，与此前 H202 布局一致 |
| 外置 Flash | QSPI0（GPIO22-27），LittleFS 固定 8 MiB；`/dl` 与 `/data` 是同一卷下 `h2loader/` 的两个目录 |
| Preference | FlashDB KVDB，起始 `0x780000`，大小 `0x6000` |
| Console | UART1（GPIO0/1），460800 baud；ROM 下载走 UART0（GPIO10/11） |

权威输入是 `boards/h202/bk7258/ap.defaults`、`cp.defaults` 和 `layouts/{loader,mfg}/` 下的 defaults 与 `partitions/bk7258/auto_partitions.csv`。App 窗口不小于 Loader 窗口，满足 [BK7258 布局要求](../../firmware_types#新-board-接入要求)。

外置 Flash 和 NFC、RGB 都由 GPIO54 控制的 `WAKE_3V3` 供电，板级在挂载存储前先打开它。挂载失败时 Loader 保留诊断并有限频率重试，不自动格式化，也不改用其他介质。原理图把 SD NAND 和 SPI Flash 列为二选一，本板只支持 QSPI Flash；原理图列出 128/256 Mbit 器件，而早期设备读到的 JEDEC 为 `0x856017`，卷容量固定为 8 MiB，启动日志会打印实际 JEDEC。

## 安装与恢复

H2Loader 可通信时，查询、暂存、安装、更新和回滚都通过 H2Loader：

```sh
h2loader --no-ble --port <serial-port> status
h2loader --no-ble --port <serial-port> send --file <package.update.tar.zlib>
h2loader --no-ble --port <serial-port> reboot upgrade --monitor
h2loader --no-ble --port <serial-port> status
```

已经烧过此布局的 H202 可以用管理更新换成 gizos 镜像，不需要 ROM 重烧。只有确认无法通信或自我恢复时，才使用 `:firmware` 生成的 recovery 镜像和 `layouts/loader/bk_loader.json` 走底层恢复。

## MP_C 与 MP_J

同一次无密钥构建产出所有产物。MP_C（AES-off）直接使用上面的普通包。MP_J（AES-on）在构建后加密同一份输出：

```sh
<secret-provider> | bazel run --config=bk7258 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/h202:encrypted_recovery -- \
  --key-fd 0 --output-dir <new-absolute-dir>

<secret-provider> | bazel run //projects/h2loader/tools/bazel:encrypted_bk_app -- \
  --source <mfg-package> --metadata <mfg-partition-metadata> \
  --key-fd 0 --output-dir <new-absolute-dir>
```

`<secret-provider>` 是调用方自己的密钥来源，通过匿名管道写入 32 字节标准 key 并结束输入。`<mfg-package>` 是 `//projects/mfg/apps/h202:package` 产出的 `.update.tar.zlib`，`<mfg-partition-metadata>` 是同一次 MFG 构建的 `partition-metadata` 目录，不能使用 Loader 的元数据。本仓库不保存任何 key、key 配置或 OTP 内容；从 OTP 到标准 key 的转换及与批次的核对由调用方负责。加密失败直接报错，不会改为输出普通产物。

加密 Loader 只能用于 recovery；AES-on 或状态未知时，公共运行时拒绝 Loader 在线自升级。完整约定见 [BK7258 离线加密恢复镜像](/zh/developing/bazel#bk7258-离线加密恢复镜像)。
