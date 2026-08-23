---
pageClass: h2loader-boards-page
---

# Boards 总览

本页列出 GizOS 公开仓库中的 H2Loader 开发板支持。每个 Board 的子文档说明接线、构建、首次烧录、恢复和验证方法；产品私有 Board 不属于本仓库的公开边界。

H2Loader 统一使用 `h2loader`、`app`、`dl`、`data`、`preference` 和 `coredump` 这些逻辑分区名。ESP-IDF 与 BK7258 SDK 的物理分区名称属于各平台实现，具体容量和配置以对应 Board 目录中的 defaults、layout profiles 与 partition 文件为准。

## 公开 Board 文档

- [AMOLED](./amoled/)
- [BK7258 V3 202405](./bk7258_v3_202405/)
- [DevKit](./devkit/)
- [SZP](./szp/)
- [Waveshare ESP32-S3-A7670E-4G](./waveshare_esp32s3_a7670e_4g/)
- [Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3](./waveshare_esp32p4_wifi6_touch_lcd_4_3/)

新增公开 Board 时必须满足[固件结构分区与类型](../firmware_types#新-board-接入要求)中的容量和更新合同，并把构建与恢复步骤记录在对应 Board 文档中。
