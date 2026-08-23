# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 Audio System

## 构建

```sh
bazel build --config=esp32p4 \
  //projects/example/targets/h2loader_tar_zlib/audio-system/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

package 的 `/data/audio/music_loop.ogg` 随 `update.tar.zlib` 安装。

## Partition layout

Audio System image 使用 board 共用的 P4 partition table：2 MB `h2loader`、8 MB `app`、各 2 MB 的 `/dl` 与 `/data`，以及 64 KB `coredump`。

## sdkconfig

Audio backend 使用 I2C0 的 GPIO 7/8、I2S0 的 MCLK/BCLK/WS/DOUT/DIN GPIO 13/12/10/9/11、GPIO 53 PA enable、ES8311 `0x18` 和 ES7210 `0x40`。PCM 为 16 kHz S16LE；ES7210 启用 MIC1、MIC2 与 MIC3，其中 ADC input 2 是 0 dB playback reference。四路 raw PCM 的 lane 0 是 reference，lane 1 与 lane 3 是两颗 microphone。

ESP32-P4 rev 1.3 的 ESP-IDF 6.x build 使用 ESP-SR 2.4.7 direct AEC：两颗 microphone 以 channel-planar buffer 输入 ESP-SR，并产生 Runtime 的 mono microphone。2.4.7 提供 P4 revision < 3 与 ESP-IDF 6.x 的预编译库组合；2.4.6 在该组合下会于 CMake 阶段拒绝构建。reference 在进入 AEC 前做 16× 数字归一化，但不改变 codec PGA、microphone 回放或扬声器输出。人工 smoke loopback 使用 100% speaker volume 与 Waveshare 官方示例的 24 dB microphone PGA，mic track 保持 unity gain。command service 同时启用 BLE 与 UART iKCP。

## 预期表现

App 循环解码并播放 `/data/audio/music_loop.ogg`，同时采集双 microphone、经 AEC 与 mixer 回放，验证双 track、capture 和 playback。运行时每约一秒输出 `h2_es7210_aec: peaks ref_raw=<n> ref=<n> mic0=<n> mic1=<n> out=<n>` 与 `H2_SMOKE_AUDIO_MIC peak=<value>`；人工说话或轻敲两颗麦克风时对应 peak 必须离开静音底噪并产生可听 loopback。初始化或运行失败输出 `H2_SMOKE_AUDIO_FAIL stage=<stage> rc=<code>`；只有 audio smoke 启动成功后才确认 OTA image。
