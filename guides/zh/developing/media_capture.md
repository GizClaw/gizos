# Desktop 音视频录像

`libs/media_capture` 定义平台无关的 C capture sink；只传递已经呈现的 RGB565 画面、已经接受的 speaker PCM 和共同的 Time PAL 单调时间。Public Header 的 Doxygen 是参数、借用关系、callback 和 unregister 合同的 source of truth。

SDL3 在主线程 `SDL_RenderPresent` 成功之后提供 LVGL Display flush 所形成的 完整 framebuffer 与 brightness modulation。透明 widget 已在 LVGL 中合成到 不透明 RGB565 Display；录像保留真实合成结果，不能把窗口透明度当作亮度。 不读取浏览器、系统屏幕或截图序列。

PortAudio 在 mixer、track gain 和 speaker volume 处理之后，只复制成功写入 输出设备的 S16 PCM；麦克风和 synthetic fallback 都不会进入录像。Blocking PortAudio API 不提供每个 buffer 的 DAC callback timestamp，因此以相同的 Time PAL 读取写入前时间，加 `Pa_GetStreamInfo()->outputLatency` 的设备估计， 随后以 sample count 延续该时钟。输出 underrun 或超过 20 ms 的 gap 重新定位， 录像在 gap 中写入 silence。该证据是实际 speaker submission 与输出延迟估计， 不是麦克风回采或物理扬声器测量。

`libs/pal/providers/ffmpeg:recording` 拥有 encoder、worker 和 MP4。它使用已有 固定版本 FFmpeg 的 MPEG-4 Visual 与 AAC-LC encoder，不启动外部 ffmpeg process。 30 fps 视频与 16 kHz mono 音频使用同一 epoch。16 个完整 video snapshot 与两秒 PCM ring 在开始时分配，callback 只在锁内复制；worker 独立编码和写文件。 100 ms holdback 等待 producer 时间戳；视频按 30 fps 采样，静止时保持最后画面， 未播放的时段保留静音。缓冲容量耗尽、迟到音频与 I/O/编码错误保留非零结果， 不能静默丢音并报告成功。

MP4 每个一秒 GOP 分片（`frag_keyframe+empty_moov+default_base_moof`），使封装的 packet metadata 也保持有界。采集 buffer 为 `34 * width * height + 64000` bytes， 另有固定 encoder、scaler 和 muxer 工作空间。编码目标码率是视频 2 Mbit/s、音频 64 kbit/s，容量规划约 15.5 MB/min、0.93 GB/h，实际静态 UI 通常更小。

`libs/pal/providers/desktop/app_support:recording` 的 C lifecycle 同时借用 SDL3、 PortAudio 和时钟；Desktop launcher 启动 capture，停止生产者后呈现最后 pending frame，再调用 stop。stop 先同步注销两个来源、等待 callback 退出，再编码到停止 时间与最后已接受 DAC sample 结束时间的较大值，保持尾帧、补齐最后 AAC block、 flush 两个 encoder 并写 trailer。正常完成、窗口退出和 SIGINT/SIGTERM 的协作 取消使用相同收尾；SIGKILL、设备丢失和不可写存储不属于成功停止。

现有文件通过 exclusive create 拒绝覆盖，输出目录必须预先存在。路径与测试 产物命名由 consumer 决定，公共 provider 不读取产品环境变量或 E2E case policy。

## 验证

```sh
bazel test --config=macos_arm64 //libs/pal/providers/ffmpeg:all \
  //libs/pal/providers/sdl3:all //libs/pal/providers/portaudio:all
bazel run --config=macos_arm64 \
  //projects/example/targets/cc_binary/recording:recording-smoke -- \
  /absolute/new-output.mp4
```

Smoke 在真实 SDL window 中显示移动的 LVGL 半透明 panel，通过真实 speaker 交替播放 440/880 Hz；6 秒后或收到协作取消后关闭 MP4。必须核对音视频 track、 尺寸、时长、decoded PCM 能量、色彩/频率转换时刻与正常/取消收尾，不能以 “存在 audio track”替代非静音播放证据。无显示或 speaker 设备时报告失败。