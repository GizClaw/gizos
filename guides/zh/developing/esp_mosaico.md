# ESP-Mosaico 适配边界

ESP-Mosaico 使用 ESP32-S31。GizOS 提供公开 board、独立 SDK/toolchain 合同及诊断固件；
产品仓库继续拥有产品 App、UI、资产、设备配置和发布策略。

## 分层归属

| 层 | 职责 |
| --- | --- |
| `libs/pal` | SDK 无关的类型、错误与资源生命周期合同 |
| `native_component_src/esp-idf6.x` | 共享 ESP PAL、ES8311 codec/mixer 和原生 SDK 编译适配 |
| `boards/esp_mosaico/esp32s31` | eFuse 版本、供电与总线接线、外设实例、Runtime config、可选 camera/CDC |
| `third_party` | 固定官方 BSP 的 source-only overlay |
| `tools/bazel` | S31 平台、独立 SDK/tool 版本、native graph 与构建输出 |
| `projects/e2e/targets` | 诊断固件组合、测试 UI、按键操作与任务策略 |
| `projects/h2loader/targets` | 现有 Loader 的板级组合；构建不等于安装/恢复验收 |

公共 board 允许提供 Runtime config，实际 Runtime 生命周期和应用映射由最终入口持有。
可选 camera 通过独立 getter 获取，当前没有 Runtime camera 字段或应用层绑定。
模块管理器属于官方 BSP 集成，不新增跨平台热插拔框架。

## Camera PAL

`h2_pal_camera_api_t` 是串行、单消费者的可选接口，只有 start/acquire/release/stop。
帧内容固定为 UYVY（U0,Y0,V0,Y1），携带 data、size、width、height、stride 和不透明 token。
消费者按 stride 寻址，不能假设相邻行没有 padding。

acquire 使用 provider 文档规定的有限超时；成功后帧由消费者借用，直到 release 成功才失效。
持有帧时再次 acquire 或 stop 返回 BUSY。release 失败仍保留借用，调用者必须保留原描述符重试；
不能用仍借用的描述符作为下一次 acquire 的输出，因为 wrapper 会清空输出。
旧 token、重复归还无效。stop 成功可重复调用，失败保留后端资源用于重试。
空输出参数返回 INVALID_ARG，缺少 API/vtable/callback 返回 UNSUPPORTED。

Mosaico provider 固定左槽 1280×720 UYVY、1000 ms 获取超时；仅有效 camera 描述符允许打开。
它把上游零 bytes-per-line 规范化为 width×2，并检查尺寸、溢出和缓冲区长度。
SDK DVP 补丁作用于构建目录副本，SC101IOT 补丁作用于临时 managed component，均失败即停止构建。
消费者须使用 board 的 `camera/sdk_setup.cmake`；格式协商、多路流、编码及网络服务不在范围内。

## 磁力计和电池

`H2_PAL_IMU_HAS_MAG` 表示返回磁场读数；逐轴 `H2_PAL_IMU_MAG_*_SATURATED` 标记对应轴溢出，
该轴数值为 `H2_PAL_IMU_MAG_INVALID`。调用者必须检查 flags，不能将哨兵值参与姿态计算。
其他有效轴保留 milligauss 单位，真实总线错误仍通过非零结果返回。
这不提供磁吸方向分类器，也不将饱和解释为传感器通信故障。

BQ27220 只读取标准遥测寄存器，不调用会改写容量/CEDV 的官方初始化。
电压、电流、SOC 可读与电池标定正确分别验收。

## 验证范围

V1.2 bring-up 已有人眼/听觉确认的显示、提示音和 SC101IOT 动态预览，以及基础 Core、Audio、
Wi-Fi/BLE smoke 日志。整合后构建与历史实机证据分别记录；V1.0/1.1 接线只有版本映射测试。
摄像头实际热插入、右侧扩展功能、NAND、振动和更新回滚尚未验收。
S31 不链接缺少相应 ABI 的 ESP-SR，板级 AEC 显式关闭。

摄像头热插入仅限左侧，不支持带电拔出。右侧 GPIO 与音频共享；模块发现不能证明电气热插拔安全。
通用接口扩展需由新的跨板消费者需求驱动，不能仅凭本板诊断页增加 Runtime 或产品流程。

## 其他板子的兼容性

Camera 是独立的新增可选 API，不改变已有 Runtime config、provider vtable 或设备枚举。
未实现 camera 的板子可使用 `h2_pal_unsupported_camera_api()`，公共契约测试覆盖空 API、
空 vtable、缺少 callback 和规范 unsupported provider；不会为了接口存在而加载相机依赖。

磁力计只增加原 uint32 flags 中的位与无效轴哨兵，原 HAS_ACCEL/GYRO/MAG 值和结构布局不变。
SZP 的 QMI8658 路径只提供加速度/角速度，不设置磁场标记，desktop 模拟路径也不提供磁场。
Runtime 当前手势只消费具有相应 flag 的加速度/角速度；新增回归确保只有饱和磁场的样本
不会产生运动事件。其他板子无需捏造磁场饱和状态；未来有磁力计的 provider 必须遵守逐轴契约。
C/C++ 公共头与完整兼容 host graph 验证源码兼容，其他 MCU 的实际构建/硬件结果另行记录。
