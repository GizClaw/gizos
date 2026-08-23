# Waveshare ESP32-S3-A7670E-4G Crash Before Confirm

构建入口为 `projects/example/targets/h2loader_tar_zlib/crash-before-confirm/waveshare_esp32s3_a7670e_4g`。该诊断 App 在 H2Loader command service ready 后、image confirm 前触发 crash，用于观察 Loader 对未确认 image 的自动回滚。诊断记录应包含重启后的 `status`，用来说明设备最终运行的 image；一次 panic log 或 reboot 本身不能说明回滚后的实际状态。
