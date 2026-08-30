# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 Crash Before Confirm

## 构建

```sh
bazel build --config=esp32p4 \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

## Partition layout

Crash Before Confirm image 使用 board 共用的 P4 partition table，并将 panic coredump 写入 64 KB `coredump` partition。`app` partition 在确认前保持 OTA trial 状态。

## sdkconfig

App 启用 flash coredump、16-task 上限、2048-byte coredump stack 和 panic reboot。BLE 与 UART iKCP command service 在触发 crash 前启动，UART0 使用启动默认值 `115200` baud。

## 预期表现

App 输出 `H2_CRASH_BEFORE_CONFIRM_READY action=crash` 后主动 abort，不确认当前 image。重启后 H2Loader 必须识别未确认 trial、保留可读取的 coredump，并回退到 canonical image；随后 UART 或 BLE command transport 可重新连接。验收同时检查 coredump partition 非空、`status` 回到 idle，以及再次 power-cycle 后仍运行 canonical image。
