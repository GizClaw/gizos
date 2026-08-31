# Waveshare ESP32-S3-A7670E-4G

官网：[ESP32-S3-A7670E-4G](https://docs.waveshare.com/ESP32-S3-A7670E-4G)

仓库 board identity 为 `waveshare_esp32s3_a7670e_4g`，target 和 chip 均为 `esp32s3`。Loader 与所有 App image 通过 UART0 的 CH343 USB 串口提供 H2Loader command transport，console baud 固定为 `115200`；同时启用 BLE 5 LE H2Loader transport，不支持 Bluetooth Classic。

## 硬件边界

- A7670E AT/PPP UART 使用 ESP32-S3 GPIO18 TX 和 GPIO17 RX，baud 为 `115200`。操作 Modem 前保持板上 4G 开关为 ON；当前 BSP 不驱动不同 PCB revision 可能变化的 Modem power GPIO。
- H2Loader 与本仓库的 Modem backend 都不依赖 A7670E USB：Type-C 上的 CH343 连接 ESP32-S3 UART0，AT/PPP 使用上述板内 UART。普通诊断保持 4G ON；USB OFF 时主机不能直接枚举 A7670E USB。只有需要绕开 ESP、从主机直接访问 A7670E USB 时，才按官方说明打开 HUB、4G 和 USB 开关；这不是 H2Loader 操作路径。
- 板载麦克风和扬声器连接 A7670E 的蜂窝语音通话通道，只能通过 Modem PAL 控制拨号、接听、挂断和通话状态；它们不构成 ESP32 Audio PAL，因此该 board 不提供 Audio System image。
- 24-pin FPC camera interface 与随板 OV5640 只记录为硬件 inventory；本次不提供 Camera PAL 或 camera App，也不把 V1/V2 camera pin map 写入 Runtime contract。
- 首次设备验收必须记录 PCB silkscreen revision，不根据购买时间或外观猜测 revision。

## Images

- [H2Loader](./h2loader)
- [BLE Broadcaster](./ble_broadcaster)
- [BLE Observer](./ble_observer)
- [Modem Smoke](./modem_smoke)
- [Crash Before Confirm](./crash_before_confirm)

这些 App 都是普通诊断 App：记录各阶段实际结果，不生成汇总 `PASS`、`FAIL` 或 `SKIP` 判定。某一外设或网络阶段失败不会把 App image 本身定义为失败。
