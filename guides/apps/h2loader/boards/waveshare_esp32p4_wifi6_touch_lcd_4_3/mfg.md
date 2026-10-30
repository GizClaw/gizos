# P4 Board MFG

验收程序位于 `projects/mfg/apps/waveshare_esp32p4_wifi6_touch_lcd_4_3/`，
包含存储、显示、触摸、BOOT 按键、扬声器、麦克风、Wi-Fi、BLE、AEC、
摄像头十项验收。

2026-10-30 的实机记录为 **9/10 通过**，存储、显示、触摸、按键、
speaker、mic、Wi-Fi、实体板 BLE 对端及 AEC 均已通过。
摄像头已连续取得有效帧并显示等比例预览，仍待操作者确认动态画面、
颜色和方向；采到图像不自动算作该项通过。

```sh
bazel build --noannounce_rc --config=esp32p4 \
  //projects/mfg/apps/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

通过 H2Loader 安装生成的 `waveshare_esp32p4_wifi6_touch_lcd_4_3-mfg-esp32p4.update.tar.zlib`，
随后执行 `reboot upgrade`。验收进度保存在设备中；已通过项目可以用
`mfg retry <step> <ticket>` 单独重测。

- 显示页检查四色、亮度和方向，确认后点 PASS。
- 触摸页按顺序点五个十字，检查触点跟手；其余页面的 PASS、FAIL、RETRY
  也可以触摸操作。
- 按键页包含 POWER、BOOT、RESET 三个实体键。先检查 POWER 关机再开机、
  RESET 重启后恢复到验收页，再单独按下并松开 BOOT 三次，观察计数后确认。
  BOOT 接 GPIO35；POWER 接独立电源控制电路，RESET 接 EN，后两者通过实际
  电源/重启动作和操作者确认验收。
- 扬声器页播放测试音；麦克风页靠近板子说话，观察电平。
- Wi-Fi 使用串口配置的网络。BLE 使用另一块开发板作为对端。
- AEC 包含单机校准和人工回声、近端语音确认。
- 摄像头页显示真实采集的预览，保持原始比例并居中补边。移动镜头前的物体，
  确认画面更新、颜色、方向和比例正常后再点 PASS。

摄像头接入使用 OV5647 MIPI-CSI 模组，图像缓冲和通用内存分配使用 PSRAM。
原始采集使用 1280×960 模式，通过 P4 ISP 转为 UYVY。拔掉摄像头或驱动失败时
保留失败记录，可以重新接好后 RETRY。

MFG 的 profile revision 3 在 revision 2 的九项记录后追加摄像头，
保留此前九项结果。Wi-Fi 与 BLE 仍由板载 C6 协处理器提供；此程序更新 P4 固件。
`0.1.21` 同时修复 P4 读取本地蓝牙 MAC 导致 Loader 提前退出的问题，
设备 UID 使用 P4 自身的出厂 MAC。

`0.1.27` 在 BSP manifest 中显式覆盖 `esp_video` 间接引入的 `esp_ipa`，
固定到上游 `1f7d39e37b69be18ce977dbab83d1717abcebbe4`。
这块 rev 1.3 P4 在 IDF 6.x 下需要 `libesp_ipa_p4_less_v3.a`；
使用 rev >=3 的库会在创建 ISP pipeline 时触发非法指令异常。
构建时应核对最终 link map，确认选中了 rev <3 的库。

音频使用 16 kHz、512 samples/32ms，双麦 lane 1/3、reference lane 0，
reference 数字增益 16。`0.1.25` 把 ES8311 的 100% 输出上限从
`0xb0`（-7.5 dB）提高到 `0xbf`（0 dB），默认 mic 保留 24 dB。
AEC 页先测静音与远端回声，再提示 `SPEAK NOW`、`SPEAK OVER TONE`；
按提示持续讲话，听处理后的人声回放，再确认 PASS。

本板的独立增益扫描入口位于同一 MFG board 目录的 `gain/`：

```sh
bazel build --noannounce_rc --config=esp32p4 \
  //projects/mfg/apps/waveshare_esp32p4_wifi6_touch_lcd_4_3:gain_package
```

先保存原 Loader 对应的 update 包与分区校验值，再临时安装扫描 Loader。
Stage 清空后执行一次 `reboot loader --monitor`，保持环境安静。
程序测量 12 组 speaker/mic、两档测试音和前后静音，共 48 个窗口，
日志包含真实 raw ADC、AEC reference、处理后 PCM 和完成的 DAC 帧统计。
`gain/analyze.py` 验证 UID、版本、run、完整窗口、实时 cadence、
数字余量和回声抑制。结束后恢复原 Loader 包并核对两个分区。
扫描结果用于选候选；近端讲话、双讲和回放仍须完成 MFG AEC 的人工验收。
