# Waveshare ESP32-S3-A7670E-4G BLE Observer

构建入口为 `projects/example/targets/h2loader_tar_zlib/ble-observer/waveshare_esp32s3_a7670e_4g`。App 使用 BLE 5 LE extended scan 记录完整、分片、截断和 malformed advertising report 的诊断信息，不生成汇总测试判定。App 启动 H2Loader UART/BLE command service 后执行诊断，并独立确认 image。
