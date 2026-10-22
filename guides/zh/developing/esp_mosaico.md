# ESP-Mosaico 板级支持

本实现基于 main 固定的 ESP-IDF 6.2。应用直接依赖
`//boards/esp_mosaico/esp32s31` 并选择其 `layouts/h2loader` 配置（16 MB NOR、Loader 3 MB / App 7 MB）；无需引用
`e2e-app` 或 GizClaw 测试包。`h2_esp_board_runtime_config()` 提供板级配置，
其中 Camera 默认绑定到 SC101IOT provider。公共 Runtime 未配置 Camera 时使用
unsupported provider，其他板不自动引入 Mosaico 驱动。

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
中串行执行生命周期和借帧操作，禁止 ISR 调用；共享 libco 任务上的阻塞会影响其他
协程。provider 拒绝重叠调用，但不提供多消费者调度服务。

Runtime 创建/销毁不启动、取帧、归还或停止 Camera。应用必须在 Runtime/provider
销毁前归还全部帧并停止采集。未安装摄像头不阻止普通板级或 Loader 启动。

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

## 软件验证及 E2E 覆盖

| 包/目标 | 可以验证 | 不能据此确认 |
| --- | --- | --- |
| `//projects/e2e/targets/esp_idf_firmware/reference-smoke/esp32s31_chip:firmware` | 固定 SDK、S31 工具链、最小芯片配置及跨编译链接 | Mosaico 外设、运行时中断和实际启动 |
| `//libs/pal:test_pal_camera`、PAL C/C++ header tests | Camera 公共接口、错误、借帧描述符兼容 | 实际传感器采集 |
| `//libs/runtime:runtime_test`、`runtime_concurrency_test` | 默认 unsupported、注入描述符/上下文、无隐式生命周期及现有 Runtime 回归 | 真机调度、协处理器/栈保护 |
| `//boards/esp_mosaico/esp32s31:{camera_test,usb_test,runtime_lifecycle_test}` | 固定超时配置、借帧/清理、单 CDC 实际 VFS callback 路径及失败恢复、板级注入 | DVP 电气时序、USB 枚举/物理重连 |
| 板级 `peripherals_test`、`mag_test`、`surface_test`、`audio_lifecycle_test`、`audio_volume_test`、`fs_mount_test` | 转换/边界、失败回滚、资源所有权、NOR 挂载组合 | 实际读数、触摸、声音、Flash 写入 |
| `//projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package` | 默认 Camera/OTG CDC 的整机编译链接及 Format 1 包 | 安装、启动 App、确认、返回/恢复链路 |

可复用的现有 E2E 工作负载及使用边界如下；表内路径均位于
`//projects/e2e/apps/`，不是本次新增的 Mosaico 固件包：

| App 包 | 相关覆盖 | Mosaico 使用条件 |
| --- | --- | --- |
| `pal-core/app:pal_core_e2e` | 内存、日志、时间、Timer、Task、Queue、Sync、System Event、固件身份 | 应优先验证真实 S31 provider；需要 launcher 的资源/时间等观察接口 |
| `pal-storage/app:pal_storage_e2e` | NOR 文件与 Pref、边界、重启持久化和清理 | 需要多次独立启动及现有持久化验收入口 |
| `pal-display/app:pal_display_e2e` | RGB565、局部更新、stride、亮度、开关/重开 | 需要输出观察/采集入口；不覆盖触摸或 Camera |
| `pal-audio/app:pal_audio_e2e` | PCM 录放、音量/增益、轨道、清理及同时录放 | 需要真实音频 provider；不等于 AEC 效果或声学验收 |
| `libco/app:libco_smoke` | yield、wait/wake、timeout、cancel/join、反复上下文切换 | 后续真机验证现有 RV32 后端；host PASS 不证明 S31 栈保护正确 |
| `h2loader-serial/app:h2loader_serial_e2e` | 主机枚举、握手、只读命令；按 suite 执行安装/重连/最终身份校验 | 主机已有 `//projects/e2e/targets/cc_binary/h2loader-serial:e2e-h2loader-serial`；硬件恢复后先运行非破坏性 suite，安装链路另需 App/备份 |
| `pal-wifi/app:pal_wifi_e2e` 等网络 App | Wi-Fi 及对应网络 PAL 的补充回归 | 需网络 fixture/凭据和明确需求；不批量扩展本次包清单 |

现有 DevKit/AMOLED 固件包可以做共享行为回归，不能作为 Mosaico PASS。
当前没有独立 Camera E2E 套件，也没有为 Mosaico 组装上述所有 App 的固件入口；
不新增 `mosaico-screen`、`e2e-app/esp_mosaico` 或批量套件包。
Camera、触摸、传感器、振动、电池读数和单 CDC 混合流恢复的真机覆盖仍需后续明确
验证入口。当前 Mosaico Runtime 的 MQTT 保持 unsupported，不能直接以 `pal-mqtt`
宣称 MQTT 覆盖。Format 2 包装按需增加，不复制固件源码。

## 真机验收（暂停）

OTG CDC Loader 必须实际验证枚举和设备身份、无破坏性管理命令、持续日志混合
协议传输完整性、背压/超时、拔插及重启重连。执行前固定负载、数据量、重复次数和
恢复时限，记录自动/人工恢复差异。绑定 source/SDK/config/image hash/board UID，
保留首个失败及原固件备份，验证恢复路径和原固件还原。

Loader 自身测试不要求 App；安装后启动/确认/返回 Loader 的完整链路必须有合适
App 消费者。两者均不能由编译通过替代。硬件执行继续暂停，未经用户明确恢复不
探测、刷写或重启设备。libco 后端和共享适配暂不修改，真机若复现问题再按证据处理。
