# Audio Mixer

`libs/audio_mixer` 提供跨平台 PCM 音频混合能力。它管理多个输入 track，将音频帧混合为统一输出，并记录 master factor 和 clipping 统计。

## API Reference

[API Reference](/references/audio_mixer)

`libs/audio_mixer/include` 中实际参与项目构建的头文件是 Audio Mixer 的生产 Public API contract。它们定义 mixer 生命周期、track 创建、混合帧读取、统计，以及初始化所需的 PCM format、track queue、PAL Mem API、PAL Queue API 和 PAL Sync API。

## PAL 依赖

Audio Mixer 使用 PAL 的以下部分：

- Audio contract：复用 `h2_audio_pcm_format_t`、`h2_audio_frame_t` 和 `h2_audio_track_config_t`，并由 mixer 自己实现 `h2_pal_audio_track_t`。Mixer 不调用 `h2_pal_audio_t` 的 microphone、speaker 或 backend track 方法；传入的 `audio` 只用于返回 track 的 API identity。Track `drain` 会在 FIFO 中插入 terminal，并等待 mixer consumer 确认 terminal；调用方不能按 queue 容量或 PCM 时长 sleep 猜测播放已经排空。
- Queue API：必须注入 `h2_pal_queue_api_t`。Mixer 使用 `create`、`destroy`、`send`、`recv` 和 `close` 为每个 track 管理 PCM frame queue。
- Sync API：必须注入 `h2_pal_sync_api_t`。Mixer 使用 mutex 和 condition 保护 track slot 生命周期；关闭单个 track 时只等待该 track 的在途操作，其他 active track 和 playback consumer 继续运行。
- Memory API：可以注入 `h2_pal_mem_api_t`，用于 mixer、track state 和 mix buffer 的内存分配与释放；未注入时使用 C 标准库的 `calloc()` 和 `free()`。

## 边界

具体 codec、I2S、speaker、microphone 和 board wiring 不属于这个 library，由 component 和 BSP 提供。Audio Mixer 只处理已经符合指定 PCM format 的 frame，不负责采集、播放或驱动硬件。

Mixer 的 mutex 串行化 track slot 的创建、关闭、音量访问、non-blocking playback read、统计快照和 `close_all`。write/drain 只在短临界区登记该 track 的在途操作，随后在锁外执行可能阻塞的 Queue PAL 调用。close 先把该代 slot 标记为 closing 并关闭 PCM/drain queue，再通过 condition 等待在途操作退出；condition wait 释放 Mixer mutex，因此其它 active track 和 playback read 可以继续。

slot 只有在旧代 playback 观察结束、queue 全部成功关闭且在途操作归零后才销毁并复用。queue close 或 condition 操作失败时 slot 保持 quarantined closing 状态，后续 close/`close_all` 继续收敛；关闭完成且尚未复用时重复 close 成功。关闭后的 write、drain 和音量操作返回 `H2_AUDIO_ERR_INVALID_STATE`。

调用方必须先停止 playback、producer 和其它 Mixer/track API，再调用 `h2_audio_mixer_deinit()`。deinit 不与这些 API 并发，也不销毁借用的 PAL backend。

## 构建与测试

Bazel semantic target 为 `//libs/audio_mixer:audio_mixer`：

```sh
bazel test //libs/audio_mixer:all
```
