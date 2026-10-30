# Waveshare ESP32-S3-Touch-AMOLED-1.75C

Board identity 为 `waveshare_esp32s3_touch_amoled_1_75c`，target 为 `esp32s3`。
这是 32 MiB Flash / 8 MiB octal PSRAM 的 1.75C；现有 `amoled` 是
368×448、SH8601/FT3168 的 Waveshare 1.8，不能共用固件或 GPIO 配置。

硬件依据：[官方产品文档](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C)、
[官方示例的 pin_config.h](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/blob/4bfdc80eedbf95031e384610796bed15e0240312/examples/arduino/libraries/board_config/pin_config.h)、
[官方原理图](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf)
和 [Waveshare Board Manager 配置](https://github.com/waveshareteam/waveshare_boards/tree/main/boards/esp32_s3_touch_amoled_1_75c)。

## 硬件配置

| 外设 | 配置 |
| --- | --- |
| MCU / 存储 | ESP32-S3R8，32 MiB QIO Flash、8 MiB octal PSRAM；均使用 80 MHz |
| USB | 原生 USB Serial/JTAG，GPIO19 D− / GPIO20 D+；console 115200 baud |
| 共享 I2C | Port 0，SDA GPIO15、SCL GPIO14 |
| CO5300 AMOLED | 466×466，QSPI SPI2，40 MHz；CS 12、CLK 38、D0/1/2/3 4/5/6/7、RST 1；X gap 6 |
| CST9217 触摸 | I2C 7-bit `0x5a`，RST 2、INT 11；按官方配置镜像 X/Y；PAL 提供单触点 down/move/up |
| AXP2101 | I2C `0x34`；PWR 键通过 PMIC edge status 读取，支持软件关机 |
| BOOT | GPIO0，低电平按下；Runtime button 名为 `boot` / `power` |
| QMI8658 | I2C `0x6b`，通过已有 portable driver 暴露加速度、陀螺仪 |
| 音频 | ES8311 `0x18` + ES7210 `0x40`；I2S MCLK 16、BCLK 9、WS 45、DOUT 8、DIN 10、PA 46 |

所有 I2C 地址均为 7-bit。音频为 16 kHz、单声道 PAL 输入/输出，MCLK
为采样率的 384 倍；ES7210 MIC1/MIC2 接双麦，MIC3 接 DAC 回声参考。
共享 audio provider 的 32-bit stereo transport 中，双麦在 raw lane 1/3，
参考在 lane 0。默认 direct AEC 使用双麦克风（lane 1/3）、16 kHz / 512-sample
帧、normal NLP；mic PGA 37.5 dB，独立 reference PGA 12 dB，reference 数字倍率 1。
配置中的整数 38 表示 ES7210 最高的 37.5 dB 档位。
持续麦克风／AEC worker 固定在 CPU1，避免与 CPU0 的 Wi-Fi／网络任务争用。
单机探针可临时比较单麦 AEC。默认双麦路径的选择不代表波束形成或
近端／双讲声学资格。
首次音频使用前只设置 ALDO1 为 3.3 V 并使能该路，保留其它供电和电池充电配置。
DIN GPIO10 在初始化及重启交接时保持输入；ESP-IDF 的 RX 路由设置不会
自动清除上一镜像留下的输出使能，不能把 ADC 输出线作为 GPIO 拉低。
扬声器使用显式 0–100% 音量曲线，最大 DAC 增益为 `0xbf`（0 dB）。
独立 AMOLED 声源参与的固定摆位校准在 speaker/mic 100% 下完成两轮，
实际 ADC、AEC 和 DAC 窗口均通过，未观测到削波。该结果绑定测试电平、
声源与摆位，不代表任意距离或输入声压下的人声认证。

独立 `aec-probe` App 提供单机三音／静音基线和增益扫描，采用真实
direct AEC 与 512-sample 帧，比较单麦／双麦和独立 reference PGA。
入口和日志分析见
`projects/e2e/targets/h2loader_tar_zlib/aec-probe/waveshare_esp32s3_touch_amoled_1_75c/README.md`。
单机 far-only 结果仅用于回声、通道和 headroom 调参；完整 AEC Calibration
仍需独立近端声源检查声音保留和双讲。

显示层在 PSRAM 保存一帧 RGB565 shadow，内部 SRAM 的 DMA buffer 从 32 行
递减至 8 行。RGB565/RGB888/RGB444、裁剪和带 stride 的局部更新均通过
shadow 转换；向面板发送时将区域向外对齐到偶数像素，保留周围已有像素。

RTC、额外扩展 GPIO、充电参数配置和电池电量 PAL 暂未接入。

## 构建与验证

```sh
. /path/to/firmwares-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/waveshare_esp32s3_touch_amoled_1_75c:package \
  //projects/example/targets/h2loader_tar_zlib/display/waveshare_esp32s3_touch_amoled_1_75c:package \
  //projects/example/targets/h2loader_tar_zlib/audio-system/waveshare_esp32s3_touch_amoled_1_75c:package

bazel test --config=macos_arm64 \
  //boards/waveshare_esp32s3_touch_amoled_1_75c/esp32s3:display_buffer_test
```

显示与音频 App 使用既有跨平台 smoke App。显示 App 应呈现八色竖条并完成
App 确认；音频 App 的操作和预期行为见 [Example Apps](/apps/example)。
两个 App 均先启动 H2Loader serial/BLE command service，再运行测试并确认 App。

本接入的串口探测记录：2026-10-28，macOS 出现较新设备
`/dev/cu.usbmodem1401`，USB VID:PID 为 `303a:1001`，读取到
`esp_psram: SPI SRAM memory test OK`。USB 描述符不能单独证明具体板型；
端口名也可能在重新插接后变化。随后已完成 H2Loader 安装并验证 32 MiB Flash / 8 MiB PSRAM。实机
PAL Display 两次启动均通过 24/24，完成 SPI DMA 数据校验；用户确认
红、绿、蓝、白四象限可见。PAL Audio 两次启动均通过 24/24，含 30 秒
连续采集/播放及非零麦克风采样。这轮 PAL/GizClaw 完整验收使用调高增益前的
24 dB mic / `0xb0` DAC 配置；上述最终增益另经声学校准和 MFG 验收，
未把它算作全部 E2E 的重新执行。

截至 2026-10-30，整板 MFG 已实机记录 10/10 通过，包括五点触摸、
BOOT/PWR 按下与松开、修正后的 IMU 屏幕方向、扬声器、麦克风、
实体板 BLE 对端和 AEC 人声/双讲回放。屏幕亮度的绝对光学测量、
IMU 绝对标定及不限输入电平的音质认证不在此验收范围。

13 个现有 PAL E2E 和 GizClaw ALL 的独立入口均位于
`projects/e2e/targets/h2loader_tar_zlib/<image>/waveshare_esp32s3_touch_amoled_1_75c`。
网络套件使用现有 fixture Bazel 配置；Wi-Fi 需要独立 seed 与后续合格镜像，
版本以 `pal-wifi-` 开头；IPv6 使用同一 session 的 AMOLED 无线对端。
GizClaw 默认使用完整 ALL suite，包含 Resource/Firmware 等八个顶层域，
并启用 BLE Binding 实际对端窗口（操作见 `projects/e2e/apps/gizclaw/README.md`）；
Lua Apps 与完整 OTA_ONLY 流程使用独立开关，需要另外的 fixture。
私有 Wi-Fi 和服务器凭据通过本地 Settings / 私有 Bazel rc 注入，不写入源码。

[H2Loader 分区、首次安装和恢复](./h2loader)
