# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3

官网：[Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm)

该 board 由 ESP32-P4 host 与 ESP32-C6 wireless coprocessor 组成。P4 运行 Runtime、H2Loader、display 和 audio；板载 C6 运行固定的 ESP-Hosted slave firmware，通过 SDIO 向 P4 提供 Wi-Fi 与 Bluetooth controller。本仓库只构建和更新 P4 image，不提供或更新 C6 firmware。

- [H2Loader](./h2loader)
- [Display](./display)
- [Audio System](./audio_system)
- [MP4 Player](./mp4_player)
- [MP4 Player Small](./mp4_player_small)
- [Crash Before Confirm](./crash_before_confirm)

## P4 transport

P4 使用 SDIO 连接固定固件的 C6；ESP-Hosted 将 remote Wi-Fi 和 NimBLE VHCI 暴露给 Runtime。H2Loader command service 同时支持 BLE iKCP 与 UART iKCP，UART0 固定为 `230400` baud。Hosted 未 ready 时，UART 仍是 P4 恢复路径；不要通过本项目的构建或恢复流程改写 C6。
