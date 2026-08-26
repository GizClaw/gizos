# MP4 Player

BK7258 的 MP4 Player image 位于 `projects/example/targets/h2loader_tar_zlib/mp4-player/bk7258_v3_202405/`。AP Launcher 使用 PSRAM allocator，接入 `h2_tinyh264` Video Decoder PAL、BK Helix AAC-LC provider、board Audio PAL 和 Display PAL；CP 保留 H2Loader command transport。

portable App 使用三个循环复用的 presentation slot；App 调用线程负责视频呈现，decoder 和 Audio writer 分别使用独立 Runtime task。Display 或 Audio PAL 阻塞时，decoder 可以继续准备下一个空闲 slot；slot 只有在音频和视频两个 consumer 都释放后才会再次进入 free queue。

Package 中的 `/data/media/showcase.mp4` 是 800×480、15 fps 的 H.264 Constrained Baseline 视频，包含 16 kHz mono AAC-LC 音频。App 在第一帧成功显示后确认 H2Loader image，并持续循环播放音视频。

## 构建

```sh
bazel build --config=bk7258 \
  //projects/example/targets/h2loader_tar_zlib/mp4-player/bk7258_v3_202405:package
```

构建生成 `bazel-bin/projects/example/targets/h2loader_tar_zlib/mp4-player/bk7258_v3_202405/package/example-mp4-player-bk7258_v3_202405.update.tar.zlib`。安装与状态确认使用 [H2Loader CLI](/zh/using/h2loader/cli)，不得绕过仍可通信的 H2Loader 执行底层烧录。

## 验收

- 屏幕以 800×480 方向显示视频，没有旋转或拉伸。
- 扬声器播放与视频素材对应的声音。
- 六秒素材结束后音视频继续循环。
- `status` 返回 `active_role=app`、`active_name=mp4-player`、`state=confirmed`，且 `staged_valid=0`。
