# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 Display

## 构建

```sh
bazel build --config=esp32p4 \
  //projects/example/targets/h2loader_tar_zlib/display/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

## Partition layout

Display image 使用 board 共用的 P4 partition table：2 MB `h2loader`、8 MB `app`、各 2 MB 的 `/dl` 与 `/data`，以及 64 KB `coredump`。

## sdkconfig

Display 使用 480 × 800 RGB565 ST7701 MIPI-DSI panel、两条 500 Mbps data lane、30 MHz DPI clock、LDO channel 3 和 GPIO 26 inverted backlight。command service 同时启用 BLE iKCP 与 `230400` baud UART iKCP，panic coredump 写入 flash。

## 预期表现

启动后屏幕亮度为 90%，从左到右显示白、黄、青、绿、紫、红、蓝、黑八条竖向色带。成功绘制后 App 确认当前 OTA image；任何失败输出 `H2_SMOKE_DISPLAY_FAIL stage=<stage> rc=<code>`，不得确认 image。
