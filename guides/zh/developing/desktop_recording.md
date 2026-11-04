# Desktop 音视频录像

`//libs/desktop_recording` 是 Linux/macOS Desktop 专用的公共录像 library。它提供直接的 C create/hooks/stop/destroy 函数与 opaque handle，作为 Desktop capture hooks 的一个消费者，拥有编码、MP4 和编码器生命周期；没有 PAL capability、API object 或 vtable。Public Header 的 Doxygen 是借用关系、并发与停止语义的 source of truth。

SDL3 的直接 frame callback 在主线程 `SDL_RenderPresent` 成功之后提供 LVGL Display flush 形成的完整 RGB565 framebuffer 与 brightness modulation。透明 widget 已在 LVGL 中合成到不透明 Display；录像保留实际合成结果，不能把窗口透明度当作亮度。不读取浏览器、系统屏幕或截图序列。

PortAudio 的直接 output callback 在 mixer、track gain 和 speaker volume 处理之后，只复制成功写入输出设备的 S16 PCM；麦克风和 synthetic fallback 都不会进入录像。Display、speaker 与 recorder 全部使用 native `std::chrono::steady_clock` 的同一时间轴。Blocking PortAudio API 不提供每个 buffer 的 DAC callback timestamp，因此以写入成功返回后的时间加 `Pa_GetStreamInfo()->outputLatency` 的设备估计，包含等待设备 buffer 空间的阻塞时间，随后以 sample count 延续该时钟。输出 underrun 或超过 20 ms 的 gap 重新定位，录像在 gap 中写入 silence。该证据是实际 speaker submission 与输出延迟估计，不是麦克风回采或物理扬声器测量。

Recorder 直接使用已有固定版本 FFmpeg 的 MPEG-4 Visual 与 AAC-LC encoder，不启动外部 ffmpeg process。30 fps 视频与 16 kHz mono 音频使用同一 epoch。64 个 video snapshot 槽与可保存两秒实际 PCM 样本的带时间戳队列在开始时分配，callback 只在锁内复制；encoder worker 编码并顺序封装，独立 writer 线程通过固定 4 MiB 的已编码字节队列持续写文件。磁盘短暂阻塞时，encoder 继续消费采集队列，写盘中的字节仍占用输出队列容量，未写完前不能复用。已编码队列满或写入失败都会保留错误，不能静默丢片段。音频容量只计算尚未编码的样本，调度间隔不占用队列；worker 按时间戳补静音，因此编码线程短暂停顿后不能把空队列误判为 FULL。100 ms holdback 等待 producer 时间戳；视频按 30 fps 采样。同一输出采样时刻之前的连续呈现只保留最后一份快照，这与编码器原本取该时刻最新画面的行为一致。64 个采样槽提供约两秒的有界调度余量，静止时保持最后画面，未播放的时段保留静音。实际排队样本或画面容量耗尽、迟到音频与 I/O/编码错误保留非零结果，不能静默丢音并报告成功。

MP4 每个一秒 GOP 分片（`frag_keyframe+empty_moov+default_base_moof+skip_trailer`），并关闭需要累计整段 fragment index 的可选 mfra/tfra trailer，使封装的 metadata 也保持有界。每个片段自身保留时间戳，输出不支持 seek；正常 stop 调用 muxer finalization 刷出最后片段，再等待 writer 排空并关闭文件，全部成功后才返回 OK。`buffer_bytes` 包含采集及编码输出队列，共 `130 * width * height + 320000 + 4194304` bytes，其中 PCM 占 64000 bytes，样本时间戳占 256000 bytes；另有固定 encoder、scaler 和 muxer 工作空间。编码目标码率是视频 2 Mbit/s、音频 64 kbit/s，容量规划约 15.5 MB/min、0.93 GB/h，实际静态 UI 通常更小。

Desktop launcher 先创建 recorder，再把 `h2_desktop_recording_hooks()` 返回的回调放入 `h2_desktop_capture_config_t.hooks` 并注册。Recorder 的 `on_mic` 为 NULL，MP4 音轨只来自 speaker。停止生产者并呈现最后 pending frame 后，launcher 先 destroy capture registration，等待 callback 退出，再 stop recorder。编码器写到停止时间与最后已接受 DAC sample 结束时间的较大值，保留最后 presentation、保持尾帧、补齐最后 AAC block、flush 两个 encoder 并写 trailer。正常完成、窗口退出和 SIGINT/SIGTERM 的协作取消使用相同收尾；SIGKILL、设备丢失和不可写存储不属于成功停止。

The Linux runtime allowlist includes `libavformat.so.62` from the pinned FFmpeg
source build. The integration test checks the recorder smoke binary and the PAL
Display E2E binary with `ldd`; running that closure check requires a matching
Linux host.

`h2_desktop_recording_result()` 可在录制期间读取锁存结果，OK 只表示目前未观察到错误；最终结果仍由 stop 返回。首次错误会立即停止接受新媒体，并在输出仍可写时让编码 worker 按当时已经接受的尾帧/尾音收尾关闭文件，不能等应用稍后退出时再把静帧和静音延长到那个时间。Desktop 控制循环发现错误后先注销 hooks，再停止生产者。首次错误诊断记录来源、当时采集和输出队列占用、编码 cursor，不包含像素或 PCM 内容。

现有文件通过 exclusive create 拒绝覆盖，输出目录必须预先存在。路径与测试产物命名由 consumer 决定，公共 library 不读取产品环境变量或 E2E case policy。

## 验证

```sh
bazel test --config=macos_arm64 //libs/desktop_recording:all \
  //libs/pal/providers/sdl3:all //libs/pal/providers/portaudio:all \
  //libs/pal/providers/desktop/app_support:capture_test
```

The retained Linux runtime integration fixture is `//tools/bazel/linux_runtime/tests/recording:recording-smoke`. It belongs to the build tool's runtime tests and is not a public sample App. With real display, microphone and speaker devices available, it shows a moving translucent LVGL panel, alternates 440/880 Hz speaker output and counts microphone samples through a separate hook without recording them. It closes the MP4 after six seconds or cooperative cancellation. Acceptance checks tracks, dimensions, duration, decoded PCM energy, color/frequency transition timing and normal/cancelled cleanup; an audio track alone does not prove non-silent playback.

```sh
bazel run --config=linux_x86_64 \
  //tools/bazel/linux_runtime/tests/recording:recording-smoke -- \
  /absolute/new-output.mp4
```

自动回归使用真实 POSIX pipe 阻塞 writer 超过两秒，同时持续提交画面与 PCM；恢复写入后解码 MP4，核对阻塞期间的音频与画面。另验证输出队列耗尽、ring wrap 字节顺序、断开的输出、首次错误停止及延迟应用退出不会延长失败录像。长录验收必须在真实 Desktop E2E 运行期间持续读取完整 MP4 分片，检查时间戳推进、可解码画面和音频，并在结束后核对整个时段；文件存在或大小增加不能单独作为成功证据。
