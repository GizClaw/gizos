# JieLi AC791N DevKit

`jieli_ac791n_devkit` board 基于杰理 AC791N（wl82，双核 pi32v2），提供以下 H2Loader image：

- [H2Loader](./h2loader)
- [Crash Before Confirm](./crash_before_confirm)

Loader 与 color-bar App 同时启用 UART iKCP 和 BLE iKCP。UART 管理链路是板上标为 UART0 的串口（SDK 设备名 `uart1`，TX PB3 / RX PA6），固定 460800 8N1。杰理工具链只提供 Linux x86_64 二进制，macOS 开发者在 Linux x86_64 dev container 内构建，见 [JieLi Components](/zh/developing/components/jieli)。
