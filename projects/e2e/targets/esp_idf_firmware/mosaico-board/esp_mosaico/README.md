# Mosaico board E2E

独立诊断固件消费公共 board、Runtime 和共享 PAL Core/Audio suite，不依赖 H2Loader。
最终入口拥有任务策略、测试流程和诊断页；它不是产品 UI，也不代表完整整板资格认证。
适配范围跟进 [GizOS #708](https://github.com/GizClaw/gizos/issues/708)。

## 构建与烧录

按 [board 说明](../../../../../../boards/esp_mosaico/README.md) 准备固定 S31 SDK 和工具：

```sh
bazel build --config=esp32s31 //projects/e2e/targets/esp_idf_firmware/mosaico-board/esp_mosaico:firmware
```

输出目录为 `bazel-bin/projects/e2e/targets/esp_idf_firmware/mosaico-board/esp_mosaico/firmware/`。
使用 S31 SDK 的 Python 环境，在 ROM 下载模式写入该目录的合并镜像：

```sh
python -m esptool --chip esp32s31 --port "$MOSAICO_PORT" --no-stub write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 combined_factory.bin
```

`MOSAICO_PORT` 由当前设备枚举结果指定。按住 BOOT、点按 RESET、松开 BOOT 可进入下载模式。
镜像覆盖原固件及分区表；本诊断布局的 dl/data LittleFS 挂载失败时允许格式化，不能保证保留原数据。
应用启动后主 Type-C 重新枚举 TinyUSB CDC，端口可能变化；以 115200 记录完整启动日志。
15 秒倒计时后运行测试；Audio 同时录放约 30 秒，之后进入持续交互页。

## 屏幕和按键

- 显示 Core 各项结果、触摸坐标、AI/BOOT 状态、BMI270、两颗 BMM150 和只读电池数据。
- 展示 BAT、Audio、Wi-Fi、BLE、Camera 和左右模块发现计数；错误保留返回码。
- AI 切换持续摄像头预览与诊断页；BOOT 重播三声提示音。
- 亮度和提示音音量为 100%，属于本测试入口的操作策略。
- Camera 自动测试执行两轮启停、每轮 20 帧。持续预览单独统计帧数，返回诊断页释放资源。
- MAG2 的饱和轴显示 SAT，保留其他有效轴；SAT 不等于通信失败，也不表示恢复量程内测量。

## 完整资格验收状态

**当前未通过完整 Mosaico E2E 验收。** 本入口是交互式 bring-up 诊断，Core 8/8 不是新版 PAL Core 41 项资格测试。
CI 通过只能证明其配置中的构建和主机测试通过。所有实板结果必须绑定最终源码、固件包与镜像 SHA；
其他板子的通过记录、历史 bring-up、编译成功、NOT_RUN/BLOCKED 或重放旧日志均不能代替实际执行。
完整清单和设备依赖见 [Mosaico E2E qualification](../../../h2loader_tar_zlib/esp_mosaico-qualification.md)。

## 测试边界

| 项目 | 覆盖 | 不包含 |
| --- | --- | --- |
| Core | Time、Timer、Task、Queue、Mutex、Semaphore、Condition、Concurrency 基础 8 项 | 完整 PAL Core 资格压力测试 |
| Display / Input | PAL 传输、RGBW 色条、触摸事件和按键 | 颜色标定、全屏坐标标定、长时间压力 |
| IMU / MAG | 读数、单位、双磁力计自检及恢复、逐轴饱和状态 | 磁吸方向分类、传感器标定 |
| BAT | 只读标准寄存器、字段和无效参数 | CEDV 写入、SOC/容量标定 |
| Audio | 共享 24 项 E2E、30 秒同时录放、三声提示音 | AEC、声学质量或参考回路资格 |
| Wi-Fi | MAC、扫描、WPA2 AP 启停 | STA 关联、IP/DHCP、DNS、对端接入 |
| BLE | Host 启停、扫描、广播 | 配对、连接、GATT、对端接收 |
| Camera | SC101IOT 采集、UYVY 预览、借还帧和两轮清理，共 17 项 | 其他 sensor 验收、编码、Runtime camera 绑定 |
| Expansion | 官方模块管理器去抖、描述符 CRC、左右插入/移除计数 | 通用扩展驱动、右侧 GPIO 回环、电气热插拔验收 |

`H2_MOSAICO_CORE_SUMMARY`、`H2_MOSAICO_AUDIO`、`H2_MOSAICO_CAMERA_E2E` 输出自动测试结果。
`H2_MOSAICO_CAMERA_VIEW` 记录手动预览启停和清理；`H2_MOSAICO_HOTPLUG` 记录模块状态。
初始已有模块不计为热插。显示调用成功与实际画面可见、PCM 帧成功与实际发声分别验收。
NAND、振动及更新回滚尚未实测，不能由其他测试代替。

## 热插入操作

摄像头只在左侧支持热插入，**不支持带电拔出**。
先关机取下摄像头，再开机，等待日志确认左槽 ABSENT 后插入。
应观察到插入计数增加、有效 camera 描述符和新一轮采集结果。
右侧只做模块发现；其 GPIO 与音频共享，当前不执行 GPIO 回环。
存在事件或计数不等于热插入验收，必须实际观察空槽到插入再采集的过程。

## 历史硬件证据

2026-10-09 的 V1.2 bring-up：Core 8/8、Audio 24/24；用户确认三声提示音、
RGBW 色条和 SC101IOT 动态画面可见，摄像头可以使用。
Camera 17/17、40 帧，1280×720、stride 2560；两次 AI 预览退出分别 13/7 帧，清理返回 0。
Wi-Fi 扫描/AP 与 BLE 扫描/广播 smoke 通过，触摸事件及传感器读数已收到。

这些结果来自整合前烧录的固件，不能代替当前 PR 最终源码的实机回归。
实际热插入、右侧扩展功能、NAND、振动、AEC 和安装更新回滚仍未验收。
早期 bring-up 曾调用上游电池初始化并改变测试板的容量配置；当前代码已改成只读，
该测试板原完整参数未备份、未恢复，SOC 读数不能证明标定正确。
