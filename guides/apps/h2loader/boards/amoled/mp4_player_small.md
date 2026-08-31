# MP4 Player Small

AMOLED 的 `mp4-player-small` image 播放共享的 240×240 奥特曼 startup MP4。文件同时包含 H.264 Constrained Baseline 视频和 16 kHz mono AAC-LC 音频。Portable App 先把 368×448 面板清为黑色，再按原尺寸居中显示视频，不做缩放。

Launcher 使用 PSRAM allocator、TinyH264 Video Decoder PAL 和 ESP Audio Codec AAC-LC provider。Portable App 使用三个 reusable presentation slot，让解码、音频输出和视频显示可以并行推进。

## 构建

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/mp4-player-small/amoled:package
```

构建生成 `bazel-bin/projects/example/targets/h2loader_tar_zlib/mp4-player-small/amoled/package/amoled-mp4-player-small-esp32s3.update.tar.zlib`。必须通过仍可通信的 H2Loader 安装，不得绕过 H2Loader 直接烧录。

## 验收

- 240×240 动画在黑色背景上居中，没有裁剪、拉伸或旋转。
- 扬声器播放 MP4 内嵌的 startup 音频。
- 4.667 秒素材结束后音视频持续循环。
- `status` 返回 `active_role=app`，active identity 与 package manifest 一致，运行 Partition 2、Partition 2 metadata valid 且 Stage invalid。
