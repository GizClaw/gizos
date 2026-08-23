# MP4 Decoder

`libs/mp4_decoder` 是 target-independent ISO BMFF composition layer。它从调用方提供的同步 random-access `read_at` source 读取非加密、非 fragmented MP4，解析 H.264/AAC sample table，并驱动注入的 Video Decoder 与 Audio Decoder PAL。V1 在 `open` 期间把不超过 64 MiB 的 source 读入 allocator-backed storage，随后不再调用 source；文件、内存、flash 或 range adapter 的句柄和关闭动作仍由调用方负责。

Library 返回一个 borrowed presentation frame：视频 plane 已复制到 decoder 自己的 reusable writable allocation，PCM 是同一视频 PTS interval 的 interleaved S16LE samples。调用方可以安全地在视频 buffer 上叠加内容，然后将 PCM 写入 Audio PAL、将视频写入 Display PAL。Library 不持有文件句柄、Audio PAL、Display PAL、wall clock 或后台 playback task。

压缩 packet 按 DTS 提交，H.264 `ctts` composition offset 保留为 PTS。Video provider 负责 codec reorder；MP4 Decoder 以 decoded frame PTS 切分 PCM。调用方 release presentation frame 后 storage 才能复用。Loop 由调用方调用 `h2_mp4_decoder_seek(decoder, 0)` 实现。

需要让 decode、Audio PAL write 和 Display PAL present 重叠时，playback consumer 应把 borrowed presentation frame 复制到自己的有界 slot pool，再立即 release decoder frame。Slot 的 queue、consumer ownership、A/V pacing 和 display composition 属于 player 或 App；不能放回 MP4 Decoder，也不能让 worker 在 decoder frame release 后继续引用其 plane 或 PCM。

默认限制为 200,000 个 sample、16 MiB decoded PCM 和 16 MiB 单个 presentation frame；config 中对应的非零字段可以收紧或放宽，但不会表示无限制。V1 会在 `open` 中预解码完整 AAC track 到这块有界 PCM storage，因此超出限制的长音频会明确失败，而不是无界增长。A/V、video-only 和 audio-only policy 均可通过是否提供对应 provider 以及 `require_video` / `require_audio` 表达。

```sh
bazel test //libs/mp4_decoder:all
```
