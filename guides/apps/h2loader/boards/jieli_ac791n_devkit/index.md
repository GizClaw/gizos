# JieLi AC791N DevKit

`jieli_ac791n_devkit` board 基于杰理 AC791N（wl82，双核 pi32v2），提供以下 H2Loader image：

- [H2Loader](./h2loader)

Loader supports UART iKCP and BLE iKCP. Its UART management link is the board's UART0 connector (SDK device `uart1`, TX PB3 / RX PA6), fixed at 460800 8N1. The JieLi toolchain provides Linux x86_64 binaries; macOS developers build in a Linux x86_64 dev container. See [JieLi Components](/zh/developing/components/jieli).
