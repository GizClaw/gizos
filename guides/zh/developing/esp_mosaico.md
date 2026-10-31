# ESP-Mosaico 板级支持

本实现基于 main 固定的 ESP-IDF 6.2。应用直接依赖
`//boards/esp_mosaico/esp32s31` 并选择其 `layouts/h2loader` 配置（16 MB NOR、Loader 3 MB / App 7 MB）；无需引用
`e2e-app` 或 GizClaw 测试包。`h2_esp_board_runtime_config()` 提供板级配置，
其中 Camera 默认绑定到 SC101IOT provider。公共 Runtime 未配置 Camera 时使用
unsupported provider，其他板不自动引入 Mosaico 驱动。

## Loader 与 MFG 最终组合

同一块板的两个管理镜像均使用既有 `role=h2loader` 和 S31 layout：

- [普通 Loader](/apps/h2loader/boards/esp_mosaico/h2loader)：
  `//projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package`，入口保持不变。
- [MFG](/apps/h2loader/boards/esp_mosaico/mfg)：
  `//projects/mfg/apps/esp_mosaico:package`，在 `main/profile.c` 声明十二项顺序、
  外设 ID 与工厂交互策略；`main.c` 只接线共享 MFG/Loader 生命周期和命令。

二者使用 `--config=esp32s31` 构建并沿用现有 Type-C USB console/Loader adapter。 MFG 不启用 BLE 命令服务，BLE 验收仅扫描。MFG 以 `profile.total` 打开共享 全项 PASSED 门控；失败、跳过、未测、结果不可读或保存失败不能允许 App 安装/启动。 Loader 替换/恢复优先，使用既有 CLI 验证管理更新，参见[Mosaico 总览](/apps/h2loader/boards/esp_mosaico/)。

MFG 使用现有 `H2_MOSAICO_AUDIO_AEC=ON` opt-in，实时监听处理后的 PAL PCM， 保留麦克风增益并人工判断；普通 Loader 不随之启用 AEC。MFG image-local `sdkconfig.mfg.defaults` 设置 `CONFIG_VFS_MAX_COUNT=16`，供现有 Loader 文件系统、CDC 和 Camera 同时使用。不能据仅编译成功推断运行时资源共存通过。

共享 MFG 负责黑橙测试 UI、现有字体设施、固定顺序进度点及完成/重测页， 该入口不增加 LVGL 工作任务。Loader 的安装、失败、启动显示回调均为 NULL， 不增加品牌或安装进度界面。交接前共享 cleanup 停止监听/扫描/振动、归还 Camera 借帧并释放触摸/显示；清理失败保留资源并阻止交接，不能提前销毁 Runtime。

本次集成没有新 GizOS 包的构建、容量或真机通过证据。后续必须记录准确 commit/package SHA-256 和串口证据，验证十二项、暖态/冷态重测、全部通过门控、 MFG ↔ Loader 替换及 App 安装/启动/恢复。旧 Firmwares 记录仅作行为参考。

## Camera 调用与资源

仅支持左 H2 的 SC101IOT、1280×720 UYVY。关闭电源后安装或拆卸摄像头，
资源探测不代表热插拔安全。采集期间 BSP 负责左 EEPROM/GPIO14 和 USJ/DVP 资源互斥。
不访问 NAND，不使用 OV3640 作为回退，不修改 SDK 或外部驱动源码。

```c
const h2_pal_camera_api_t* camera = runtime->camera;
if (h2_pal_camera_start(camera) == H2_PAL_OK) {
    h2_pal_camera_frame_t frame;
    if (h2_pal_camera_acquire(camera, &frame) == H2_PAL_OK) {
        /* 按 frame.format/stride/size 使用借用数据，不保存跨 release 的指针。 */
        int rc = h2_pal_camera_release(camera, &frame);
        /* release 失败时保留 frame，并先重试成功；不要继续 stop/销毁。 */
        if (rc != H2_PAL_OK) return rc;
    }
    /* stop 失败保留 provider 状态，由调用方重试清理。 */
    return h2_pal_camera_stop(camera);
}
```

provider 创建驱动时设置 `frame_timeout_ms=1000`。这是有限的驱动取帧等待上限，
不是每次固定延迟，也不保证整个函数的硬实时期限。上游私有 mutex 使用
`portMAX_DELAY`，start/stop、锁等待及调度延迟不由该超时约束。应用在合适的工作任务
中串行执行生命周期和借帧操作，禁止 ISR 调用。provider 拒绝重叠调用，
但不提供多消费者调度服务。

Runtime 创建/销毁不启动、取帧、归还或停止 Camera。应用必须在 Runtime/provider
销毁前归还全部帧并停止采集。未安装摄像头不阻止普通板级或 Loader 启动。

## 磁力计读取与补偿输出

Mosaico 磁力计通过现有 [IMU PAL](https://github.com/GizClaw/gizos/blob/main/libs/pal/include/h2/pal/hal/h2_pal_imu.h) 读取。[板级适配器](https://github.com/GizClaw/gizos/blob/main/boards/esp_mosaico/esp32s31/src/h2_mosaico_peripherals.c) 根据 BSP 返回码决定读取成功或失败；成功时保留三轴补偿输出并统一换算为 PAL 单位，不因单轴补偿溢出而丢弃整份样本。初始化或读取失败仍返回既有错误并清空输出，不返回上次成功的样本。

Bosch 补偿溢出哨兵也经过相同单位换算，具体映射见 [板级转换定义](https://github.com/GizClaw/gizos/blob/main/boards/esp_mosaico/esp32s31/src/h2_mosaico_mag.h)。该输出不是可信的磁场测量，也不能据其符号推断物理方向。读取成功与 `HAS_MAG` 表示交付了磁场通道输出，不保证每轴均为有效物理测量，更不代表磁铁已吸附。当前公共接口没有逐轴状态字段，消费此路径的产品必须处理这一板级语义；分类、滤波、基线和显示策略归产品层，不能把读取错误或溢出输出直接当作吸附。

MFG profile 将补偿无效值 `-327680` mGauss 传入共享策略，逐轴忽略并撤销该轴 基线有效性；恢复后的首次有效值只建立新基线，不算磁铁响应。右侧 ID 202、 左侧 ID 203 分别比较有效基线、显示当前靠近/移开状态，历史响应就绪与人工结论 保持分离。共享引擎不依赖 Mosaico BSP，也不按 board 字符串分支。

[板级回归说明](https://github.com/GizClaw/gizos/blob/main/boards/esp_mosaico/esp32s31/tests/README.md)中的磁力计测试覆盖补偿输出保留、换算和真实读取失败清空。真机验证需记录固件版本、两颗外设的返回码、flags 和三轴输出，并分别观察磁铁靠近和移开后的行为；未触发补偿溢出时，不能以正常读取替代该条件的硬件证据。没有轴输出的 IO 错误不能直接归因为饱和或硬件损坏。

## Type-C OTG CDC Loader

这是 Mosaico 板载 Type-C 的适配，不是 S31 通用 USB 策略。DevKit/AMOLED 的
USB Serial/JTAG 默认路径不变。板级代码初始化一份 TinyUSB CDC0，并把 SDK/stdio、
PAL 日志和 Loader 协议输出绑定到共同 TX 锁。console 不读取 RX，Loader 是唯一
RX 消费者。Loader 在内部 console/transport 初始化前复制板级物理 IO 描述符；
context 由板级持有，生命周期覆盖整个启动周期。注入不可重复或在启动后替换。

完整协议帧的分段入队/flush 保持 TX 所有权，锁内不向相同 CDC 递归打印日志。
半帧失败返回 CLOSED，阻止后续输出并清空尚未发送的 TinyUSB TX 队列；恢复时丢弃旧 RX 数据，必须由主机
关闭/重新建立 CDC 连接后恢复。Loader transport 收到 CLOSED 后重置会话和过滤器，
不能把普通空读 TIMEOUT 当成断链。此恢复策略及实际主机 DTR/拔插行为仍需真机验证。

## PAL E2E 支持范围

本次仅交付 Mosaico 上现有 PAL 及消费者 E2E 的运行支持，具体入口、验证边界与
已通过方案见 [Mosaico PAL E2E](esp_mosaico_e2e.md)。Lingxi、OneNET 暂不接入。
板级驱动与 Camera/Runtime/USB 是测试运行所需的基础组合，不表示整板外设验收完成。

`peripherals`、`chip`、`loader` 不作为独立 E2E 包：

- 传感器、电池、按键、触摸和 PWM 绑定既有 PAL，实现在板级目录，回归归板级 tests。
- 芯片最小配置保留在 `boards/esp32s31_chip`，编译检查归 `tools/bazel/smoke/esp32s31`。
- Loader 协议绑定归 `projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_mosaico_usb`，
  真实管理/安装链路由已有 `h2loader-serial` 验证。BSP 只暴露通用单 CDC 传输。

共享 E2E App 保留原断言；Mosaico launcher 只负责板级组合、任务策略、fixture 与结果输出。
AMOLED 对端仅作为已有协议及测试声源夹具，不扩大为其他板卡的产品适配交付。
