# Waveshare ESP32-S3-Touch-AMOLED-1.75C H2Loader

## 存储与启动

全部存储位于板载 32 MiB NOR Flash；H2Loader 不依赖 SD 卡。
权威布局为 `boards/waveshare_esp32s3_touch_amoled_1_75c/esp32s3/layouts/h2loader/partition.csv`。

| 物理分区 | 容量 / 起点 | 用途 |
| --- | --- | --- |
| `nvs` | 24 KiB / `0x9000` | ESP-IDF NVS |
| `otadata` | 8 KiB / `0xf000` | OTA 选择与 rollback 状态 |
| `phy_init` | 4 KiB / `0x11000` | 射频初始化 |
| `h2loader` (`ota_0`) | 2 MiB / `0x20000` | canonical Loader，Partition 1 |
| `app` (`ota_1`) | 12 MiB / `0x220000` | App / trial Loader，Partition 2 |
| `dl` | 8 MiB / `0xe20000` | LittleFS `/dl`，压缩 package staging |
| `data` | 8 MiB / `0x1620000` | LittleFS `/data`，安装后的 App 数据 |
| `coredump` | 64 KiB / `0x1e20000` | 崩溃记录 |
| `pref` | 256 KiB / `0x1e30000` | LittleFS `/pref`，H2Loader metadata / preferences |

App raw image 必须小于等于其目标分区容量；压缩 package 及安装数据还必须
分别满足 `/dl`、`/data` 的实际可用空间，包含 filesystem metadata 开销。
Partition 2 大于 Partition 1，允许 Loader self-update 的 trial 中继。

Board defaults 选择 240 MHz CPU、80 MHz QIO Flash、80 MHz octal PSRAM、
PSRAM XIP、Wi-Fi、NimBLE extended advertising、USB Serial/JTAG console
和 Flash coredump。Layout defaults 增加自定义分区表与 App rollback。

## 构建、首次安装与恢复

```sh
bazel build --config=esp32s3 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/waveshare_esp32s3_touch_amoled_1_75c:package
```

产物使用完整 board identity `waveshare_esp32s3_touch_amoled_1_75c` 和
target `esp32s3`。Loader 使用独立的 board build counter。需要
format-2 `update.tar` 时，选择相同 image/board 的 `h2loader_zlib_tar` target。

首次安装会替换厂商固件。先根据 USB identity、物理连接和 board 型号确认
目标设备，保留所需的厂商恢复镜像；使用本 target 的 `.recovery.h2fb`，遵循
[CLI 的 destructive recovery 流程](/zh/using/h2loader/cli)。
只有首次安装或 H2Loader 无法通信的恢复才使用此流程；可通信设备的正常更新
走 `status → send → reboot upgrade → status`，见 [App 更新](../../update/app)。

设备重新连接后必须报告完整 board/target、运行分区、role、版本和 image
SHA-256/size。App 安装后应运行 Partition 2，metadata valid，Stage invalid；
Loader 自升级终态按 [Loader 更新](../../update/loader) 核对。
串口可枚举、Host 编译或压缩包生成均不能替代这些实机验收。
