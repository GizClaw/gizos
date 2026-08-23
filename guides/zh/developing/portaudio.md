# PortAudio Provider

`//libs/pal/providers/portaudio:portaudio` 把仓库固定版本的 PortAudio 适配为 Audio PAL。它不拥有 App、Desktop layout、设备模拟或 Runtime composition。

## Lifecycle

`h2_portaudio_create()` 必须收到并在 provider 生命周期内借用 Memory、Queue 与 Sync PAL，初始化 PortAudio，并创建 opaque provider instance；缺少任一 PAL 依赖都返回 `H2_AUDIO_ERR_INVALID_ARG`。provider 把同一 Sync PAL 传给内部 Audio Mixer，不创建 host-private 同步 contract。`h2_portaudio_audio()` 返回生命周期受该 instance 约束的 Audio PAL；`h2_portaudio_destroy()` 先停止并关闭 stream，再释放 mixer、queue/state，最后终止 PortAudio。consumer 必须在 provider 前销毁。

`require_real_devices` 是 composition 注入的 policy。启用时，缺失默认输入/输出设备或 stream 初始化失败必须返回错误，不能静默切换到 synthetic microphone 或 sink。实时 callback 只进行 bounded queue/data transfer，不能分配、阻塞或调用 App code。

## Validation

```sh
bazel test --config=macos_arm64 //libs/pal/providers/portaudio:all
bazel test --config=linux_x86_64 //libs/pal/providers/portaudio:all
```

Unit test 使用 provider test seam 覆盖参数、初始化失败、Audio API wiring、cleanup 和重复 lifecycle；需要真实默认音频设备的行为由显式 integration test 验证。
