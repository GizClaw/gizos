# DevKit

官网：[ESP32-S3-DevKitC-1 v1.1 用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)

`devkit` board 提供以下 H2Loader image：

- [H2Loader](./h2loader)
- [GizClaw E2E](./gizclaw_e2e)
- [Libco Smoke](./libco_smoke)
- [PAL Preference](./pal_pref)
- [WebRTC Performance](./webrtc_performance)

该 board family 为 Loader 和全部现有 H2Loader-managed App image 启用 BLE iKCP。Loader 与 App 都通过 Service Data 广播 `devkit` identity，不携带 local name；Host 显示为 `h2l.devkit`，role 和 command capabilities 由 Service Data 区分。
