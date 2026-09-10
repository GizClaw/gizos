# BK7258 V3 202405

官网：

`bk7258_v3_202405` board 包含 AP 和 CP，并提供以下 H2Loader image 和 app：

- [H2Loader](./h2loader)
- [Display](./display)
- [Audio System](./audio_system)
- [MP4 Player](./mp4_player)
- [Crash Before Confirm](./crash_before_confirm)
- [Libco Smoke](./libco_smoke)

该 board family 为 Loader 和常规 H2Loader-managed App image 启用 AP-owned BLE iKCP，并在 UART1 固定 460800 上启用 AP-owned IO Stream iKCP command session；`libco-smoke` 是仅启动 UART/iKCP command service 的诊断例外，不把 BLE advertising 健康状况纳入 libco 验收。AP 直接拥有 UART1 RX/TX，日志与协议帧共享串口；CP 不参与 H2Loader 串口转发。启用 BLE 的 Loader 与 App 通过 Service Data 广播 `bk7258_v3_202405` identity，不携带 local name；Host 显示为 `h2l.bk7258_v3_202405`，role 和 command capabilities 由 Service Data 区分；底层恢复和首次烧录仍使用 BK 串口 loader 自己的速率。

Board 具备触摸硬件，但 AP Touch PAL provider 尚未接线，因此 Runtime 当前绑定 BSP-local unavailable Touch API object。Touch operation 返回 `H2_PAL_ERR_UNAVAILABLE`，不能被解释为已支持，也不能错误标记成已确认不支持。

内部 8 MiB Flash 使用固定 Loader 与独立地址 XIP App：Loader CP 1156 KiB + AP 1224 KiB，合计 2380 KiB；App 5100 KiB，从物理地址 `0x264000` 开始。`/dl` 和 `/data` 不占内部 Flash，继续使用 SD 卡上的 FATFS 目录。
