# FFmpeg Provider

`//libs/pal/providers/ffmpeg:ffmpeg` 把仓库固定版本的 FFmpeg 适配为 Audio Decoder PAL 与 Video Decoder PAL。它不选择容器 track、不打开文件或 URL，也不拥有 player policy。

## Decoder ownership

`h2_ffmpeg_audio_decoder_api()` 提供 AAC-LC packet decoder，输出调用方 allocator-backed 的 interleaved S16LE。`h2_ffmpeg_video_decoder_api()` 提供 H.264 Annex-B packet decoder，并把 frame 转换为 PAL 请求的 CPU-readable presentation format。codec context、packet、frame、resample/scale state 和 buffered input 都属于各自 decoder session。

Session create、push/decode、flush/reset 与 destroy 必须遵守 PAL contract：输入 packet 在调用期间借用，输出 frame 的 ownership 由 PAL API 明确表达；error/EOF 不泄漏 FFmpeg object；reset 后可以重新开始一个独立 stream。Provider 不依赖 Linux、Darwin、Desktop、Mobile 或 Web backend。

## Validation

```sh
bazel test --config=macos_arm64 //libs/pal/providers/ffmpeg:all
bazel test --config=linux_x86_64 //libs/pal/providers/ffmpeg:all
```

测试覆盖 public accessor、AAC/H.264 session lifecycle、invalid packet、flush/reset、frame ownership 与重复销毁。MP4 track selection 和同步播放属于 `libs/mp4_decoder` 与 App integration test。
