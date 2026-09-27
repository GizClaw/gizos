# Desktop 音视频录像

`//libs/desktop_recording` 是 Linux/macOS Desktop 专用的公共录像 library。它提供直接的 C create/hooks/stop/destroy 函数与 opaque handle，作为 Desktop capture hooks 的一个消费者，拥有编码、MP4 和编码器生命周期；没有 PAL capability、API object 或 vtable。Public Header 的 Doxygen 是借用关系、并发与停止语义的 source of truth。

SDL3 的直接 frame callback 在主线程 `SDL_RenderPresent` 成功之后提供 LVGL Display flush 形成的完整 RGB565 framebuffer 与 brightness modulation。透明 widget 已在 LVGL 中合成到不透明 Display；录像保留实际合成结果，不能把窗口透明度当作亮度。不读取浏览器、系统屏幕或截图序列。

PortAudio 的直接 output callback 在 mixer、track gain 和 speaker volume 处理之后，只复制成功写入输出设备的 S16 PCM；麦克风和 synthetic fallback 都不会进入录像。Display、speaker 与 recorder 全部使用 native `std::chrono::steady_clock` 的同一时间轴。Blocking PortAudio API 不提供每个 buffer 的 DAC callback timestamp，因此以写入成功返回后的时间加 `Pa_GetStreamInfo()->outputLatency` 的设备估计，包含等待设备 buffer 空间的阻塞时间，随后以 sample count 延续该时钟。输出 underrun 或超过 20 ms 的 gap 重新定位，录像在 gap 中写入 silence。该证据是实际 speaker submission 与输出延迟估计，不是麦克风回采或物理扬声器测量。

Recorder 直接使用已有固定版本 FFmpeg 的 MPEG-4 Visual 与 AAC-LC encoder，不启动外部 ffmpeg process。30 fps 视频与 16 kHz mono 音频使用同一 epoch。16 个完整 video snapshot 与可保存两秒实际 PCM 样本的带时间戳队列在开始时分配，callback 只在锁内复制；worker 独立编码和写文件。音频容量只计算尚未编码的样本，调度间隔不占用队列；worker 按时间戳补静音，因此编码线程短暂停顿后不能把空队列误判为 FULL。100 ms holdback 等待 producer 时间戳；视频按 30 fps 采样，静止时保持最后画面，未播放的时段保留静音。实际排队样本或画面容量耗尽、迟到音频与 I/O/编码错误保留非零结果，不能静默丢音并报告成功。

MP4 每个一秒 GOP 分片（`frag_keyframe+empty_moov+default_base_moof+skip_trailer`），并关闭需要累计整段 fragment index 的可选 mfra/tfra trailer，使封装的 metadata 也保持有界。每个片段自身保留时间戳，正常 stop 仍调用 muxer finalization 刷出最后片段并关闭文件。采集 buffer 为 `34 * width * height + 320000` bytes，其中 PCM 占 64000 bytes，样本时间戳占 256000 bytes；另有固定 encoder、scaler 和 muxer 工作空间。编码目标码率是视频 2 Mbit/s、音频 64 kbit/s，容量规划约 15.5 MB/min、0.93 GB/h，实际静态 UI 通常更小。

Desktop launcher 先创建 recorder，再把 `h2_desktop_recording_hooks()` 返回的回调放入 `h2_desktop_capture_config_t.hooks` 并注册。Recorder 的 `on_mic` 为 NULL，MP4 音轨只来自 speaker。停止生产者并呈现最后 pending frame 后，launcher 先 destroy capture registration，等待 callback 退出，再 stop recorder。编码器写到停止时间与最后已接受 DAC sample 结束时间的较大值，保留最后 presentation、保持尾帧、补齐最后 AAC block、flush 两个 encoder 并写 trailer。正常完成、窗口退出和 SIGINT/SIGTERM 的协作取消使用相同收尾；SIGKILL、设备丢失和不可写存储不属于成功停止。

Linux runtime closure 包含由固定 FFmpeg source build 提供的 `libavformat.so.62`；其精确 SONAME 纳入 `tools/bazel/linux_runtime` allowlist，真实 recorder binary 与 Showcase binary 都经过 `ldd` closure 集成检查。

现有文件通过 exclusive create 拒绝覆盖，输出目录必须预先存在。路径与测试产物命名由 consumer 决定，公共 library 不读取产品环境变量或 E2E case policy。

## 验证

```sh
bazel test --config=macos_arm64 //libs/desktop_recording:all \
  //libs/pal/providers/sdl3:all //libs/pal/providers/portaudio:all \
  //libs/pal/providers/desktop/app_support:capture_test
bazel run --config=macos_arm64 \
  //projects/example/targets/cc_binary/recording:recording-smoke -- \
  /absolute/new-output.mp4
```

Smoke 在真实 SDL window 中显示移动的 LVGL 半透明 panel，通过真实 speaker 交替播放 440/880 Hz，并用独立 mic hook 统计真实输入 sample 数（不写入 MP4）；6 秒后或收到协作取消后关闭 MP4。必须核对音视频 track、尺寸、时长、decoded PCM 能量、色彩/频率转换时刻与正常/取消收尾，不能以“存在 audio track”替代非静音播放证据。无显示、mic 或 speaker 设备时报告失败。
