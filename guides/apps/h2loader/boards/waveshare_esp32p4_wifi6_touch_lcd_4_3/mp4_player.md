# MP4 Player

Waveshare P4 的 MP4 Player image 位于 `projects/example/targets/h2loader_tar_zlib/mp4-player/waveshare_esp32p4_wifi6_touch_lcd_4_3/`。Launcher 使用 PSRAM allocator，接入共享的 portable TinyH264 Video Decoder PAL、ESP Audio Codec AAC-LC provider、board Audio PAL 和 Display PAL。

Package 中的 `/data/media/showcase.mp4` 使用面板原生 480×800 scan、6 fps 的 H.264 Constrained Baseline 编码，并把完整的 800×480 横屏画面预旋转到 native frame；它同时包含 16 kHz mono AAC-LC 音频。6 fps target derivative 让 TinyH264 软件 provider 在 P4 上保留完整分辨率并满足实时 presentation deadline。App 必须成功解码音频和视频，并在第一帧成功显示后确认 H2Loader image，然后持续循环播放。

Portable App 使用三个 reusable presentation slot；App 调用线程负责视频呈现，decoder 和 audio writer 分别使用独立 Runtime task。Decoder 把同一 PTS 的 RGB565 与 PCM interval 复制进空闲 slot；Audio PAL write 和 Display PAL present 分别消费 slot，两个 consumer 都完成后才允许 decoder 复用。

## 构建

```sh
bazel build --config=esp32p4 \
  //projects/example/targets/h2loader_tar_zlib/mp4-player/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

构建在 target-owned `bazel-bin/.../package/` 中生成 managed package。安装与状态确认使用 [H2Loader CLI](/zh/using/h2loader/cli)，不得绕过仍可通信的 H2Loader 执行底层烧录。

## 验收

- 设备横放时显示完整的横屏画面，没有竖屏裁剪、拉伸或倒置。
- 扬声器播放与视频素材对应的声音。
- 六秒素材结束后音视频继续循环。
- `status` 返回 `active_role=app`、`active_name=mp4-player`、`state=confirmed`，且 `staged_valid=0`。
