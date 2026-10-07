# AMOLED

官网：[Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm)

`amoled` board 提供以下 H2Loader image 和 app：

- [H2Loader](./h2loader)
- [Display](./display)
- [QR Code](./qrcode)
- [Audio System](./audio_system)
- [MP4 Player Small](./mp4_player_small)
- [GizClaw Ping Speed](./gizclaw_ping_speed)
- [GizClaw OTA E2E](./gizclaw_ota_e2e)
- [GizClaw Session E2E](./gizclaw_e2e)
- [Crash Before Confirm](./crash_before_confirm)
- [Starboy](./starboy)
- [Lua Flappy Bird](./lua_flappybird)
- [Lua Cosmic Drift](./cosmic_drift)
- [Lua BloomSpeaker](./bloomspeaker)
- [Lua Runtime E2E](./lua_runtime_e2e)
- [Lua Link E2E](./lua_link_e2e)
- [iperf](./iperf)
- [iperf Server 触屏测试台](./iperf_server)
- [WebRTC Performance](./webrtc_performance)

AMOLED BSP 的 `h2_esp_board_display_configure()` 可在首次 panel 操作前设置像素时钟和 DMA chunk 上限；`dma_buffer_rows` 为 0 时保持 64 行默认值，可选 8/16/32/64 行，分配失败仍按行数减半。`iperf-server` 选择 8 行，为 Wi-Fi/lwIP 留出内部 SRAM，其他 image 的默认行为保持一致。
