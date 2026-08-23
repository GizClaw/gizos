# Waveshare ESP32-S3-A7670E-4G BLE Broadcaster

构建入口为 `projects/example/targets/h2loader_tar_zlib/ble-broadcaster/waveshare_esp32s3_a7670e_4g`。App 记录 legacy advertising、extended advertising、duration、max events 和 product beacon lifecycle 各阶段的 `state` 与 technical return code，不生成汇总测试判定。App 启动 H2Loader UART/BLE command service 后执行诊断，并独立确认 image。
