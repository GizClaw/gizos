# MP4 Player Small

Waveshare P4 的 `mp4-player-small` image 播放共享的 240×240 奥特曼 startup MP4。文件同时包含 H.264 Constrained Baseline 视频和 16 kHz mono AAC-LC 音频。Portable App 先把 480×800 面板清为黑色，再按原尺寸居中显示视频，不做缩放。

Launcher 使用 PSRAM allocator、共享的 portable TinyH264 Video Decoder PAL 和 ESP Audio Codec AAC-LC provider。Portable App 使用三个 reusable presentation slot，让解码、音频输出和视频显示可以并行推进。

## 构建

```sh
bazel build --config=esp32p4 \
  //projects/example/targets/h2loader_tar_zlib/mp4-player-small/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

构建在 target-owned `bazel-bin/.../package/` 中生成 managed package。必须通过仍可通信的 H2Loader 安装，不得绕过 H2Loader 直接烧录。

## 验收

- 240×240 动画在黑色背景上居中，没有裁剪、拉伸或旋转。
- 扬声器播放 MP4 内嵌的 startup 音频。
- 4.667 秒素材结束后音视频持续循环。
- `status` 返回 `active_role=app`、`active_name=mp4-player-small`、`state=confirmed`，且 `staged_valid=0`。
