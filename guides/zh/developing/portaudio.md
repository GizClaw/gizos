# PortAudio Provider

`//libs/pal/providers/portaudio:portaudio` 把仓库固定版本的 PortAudio 适配为 Audio PAL。它不拥有 App、Desktop layout、设备模拟或 Runtime composition。

## Lifecycle

`h2_portaudio_create()` 必须收到并在 provider 生命周期内借用 Memory、Queue 与 Sync PAL，初始化 PortAudio，并创建 opaque provider instance；缺少任一 PAL 依赖都返回 `H2_AUDIO_ERR_INVALID_ARG`。provider 把同一 Sync PAL 传给内部 Audio Mixer，不创建 host-private 同步 contract。`h2_portaudio_audio()` 返回生命周期受该 instance 约束的 Audio PAL；`h2_portaudio_destroy()` 先停止并关闭 stream，再释放 mixer、queue/state，最后终止 PortAudio。consumer 必须在 provider 前销毁。

正常 speaker stop 必须先等待 playback worker 退出，再调用 PortAudio `stop` 排空设备已接收的尾帧后 close；worker 的 availability/write failure 使用 `abort`，保证失败清理有界且后续可以重新启动。设备一次只接受部分 frame 时，worker 保留未写部分并按原顺序继续，不能丢弃 frame 尾部。

Desktop blocking output 使用默认输出设备的 `defaultHighOutputLatency` 建议值，以更深的 host/device buffer 吸收 renderer、decoder 和实时音频线程之间的短时调度抖动。`paOutputUnderflowed` 是可恢复的 write result：worker 继续播放并按 stream 计数，stream close 时输出最终 underflow count；其他 write error 仍进入 abort 清理。该策略只降低短时断粮概率，不掩盖上游长时间停止提供 PCM 的故障。

Desktop AEC 使用最多 16 帧的 playback-reference FIFO：只有完整写入 output stream 的 frame 才进入队列，队列满时丢弃最旧 reference；microphone worker 在处理 capture 前消费一个 reference，并按 `playback` 后 `capture` 的顺序调用 Speex。启动阶段、播放暂停或其他 reference queue 为空的时刻不阻塞 microphone、不合成静音 reference，该 capture 以未做 AEC 的原始 PCM 继续投递。playback worker 在正常 stop 或失败退出时、以及 microphone 停止时都会清空 reference queue 并重置 Speex 状态，speaker 重启不能消费停止前的旧 reference。

`require_real_devices` 是 composition 注入的 policy。启用时，缺失默认输入/输出设备或 stream 初始化失败必须返回错误，不能静默切换到 synthetic microphone 或 sink。实时 callback 只进行 bounded queue/data transfer，不能分配、阻塞或调用 App code。

## Validation

```sh
bazel test --config=macos_arm64 //libs/pal/providers/portaudio:all
bazel test --config=linux_x86_64 //libs/pal/providers/portaudio:all
```

Unit test 使用 provider test seam 覆盖参数、初始化失败、Audio API wiring、正常 stop 与失败 abort、partial-frame 保留、空 reference queue 的 raw-capture fallback、reference-before-capture 顺序、cleanup 和重复 lifecycle；需要真实默认音频设备的行为由显式 integration test 验证。
