# GizClaw 与 Audio System

Runtime Audio System 负责设备录音和扬声器 Track；GizClaw service 负责 Conversation 的 PCM 缓冲、Opus 编解码、BOS/EOS 和网络媒体状态。App integration 只在独立 Audio Task 中把 microphone PCM 写入 GizClaw，并把 GizClaw 回调交付的 PCM 写入播放 Track。Service callback 仍经 App main loop dispatch，不能直接修改 LVGL。

Peer connection 内的上下行 Opus RTP track、双向 Agent Event Stream、BOS/EOS 与动态 RPC DataChannel 的关系见 [GizClaw Peer Connection 传输拓扑](/apps/gizclaw/transport)。

## 对话流程

```mermaid
sequenceDiagram
    participant UI as App action
    participant Loop as App main loop
    participant Audio as Runtime Audio System
    participant Service as GizClaw service
    participant Giz as service-owned client

    UI->>Loop: StartConversation(workspace, generation)
    Loop->>Service: submit workspace setup operation
    Service->>Giz: select/confirm workspace
    Service-->>Loop: dispatch workspace-ready callback
    Loop->>Audio: start microphone
    Audio-->>Service: S16LE PCM chunks
    Service->>Giz: encode and send Opus stream
    UI->>Loop: CommitTurn / StopConversation
    Loop->>Audio: stop microphone
    Loop->>Service: submit commit/cancel operation
    Service->>Giz: commit input
    Giz-->>Service: Opus RTP
    Service->>Audio: decode PCM into the downlink Track
    Service-->>Loop: dispatch REPLY_AUDIO_STARTED once per reply
    Audio-->>Audio: speaker pump reads the Track
    Service-->>Loop: dispatch response terminal callback
    Audio-->>Loop: playback drained
    Loop-->>UI: conversation idle
```

按压式交互在 record pressed 后启动录音，released 后停止麦克风并提交 turn。自然对话在首次 click 后保持 session active，由 Audio/VAD 与 GizClaw realtime flow 推进；再次 click 或 Back 产生 cancel/stop command。

## Public API 与连接要求

| 对话集成所需信号 | Audio System 行为 | GizClaw Public API |
| --- | --- | --- |
| Workspace ready | 尚不读取麦克风 | Workspace activate result |
| 绑定音频 | 创建/绑定 PCM Track | `h2_gizclaw_pcm_track_create()` + `h2_gizclaw_service_set_track()`（或 App 自己实现的 `h2_gizclaw_track_t`） |
| 配置对话 | 不读取麦克风 | `h2_gizclaw_conversation_create(service, workspace, callback, completion, user)` |
| 输入开始 | 启动 microphone | `h2_gizclaw_service_audio_start()` |
| 输入 PCM | Audio Task 交付 16 kHz mono S16LE chunk | `h2_gizclaw_pcm_track_write()`（Track 的 uplink） |
| 输入提交（PTT） | 停止 microphone，并提交本轮输入结束 | `h2_gizclaw_service_audio_end()` |
| 回复 PCM | 扬声器 pump 从有界播放 track 读取 | Track 的 downlink：`h2_gizclaw_pcm_track_read()`；PCM 不经 callback 投递，每个 reply 的第一块 PCM 进入 Track 后 callback 收到一次 `H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO_STARTED` |
| 回复 terminal | 等待本地 playback drain 后结束本轮 | `h2_gizclaw_service_poll()` 分发的 `REPLY_DONE` event 与 completion callback |
| Cancel / 挂断 | 停止 mic、关闭输入、丢弃未播放输出 | `h2_gizclaw_conversation_cancel()`，之后 `h2_gizclaw_conversation_release()` |

Conversation 完成同时满足服务端 response terminal 和本地 playback drained。

## Audio 格式与背压

- Browser/opaque-track 模式不经过下面的 PCM/Opus packet API。Runtime 把平台 Track
  传入 `h2_gizclaw_config_t.webrtc_media_track`，GizClaw 只负责 conversation
  BOS/EOS 与控制事件，WebRTC provider 直接拥有 microphone、remote playback、codec
  和 RTP progression。Track 的布局不跨平台共享。
- Audio format 和 provider frame size 由 Runtime Audio capability 决定，App 不能写死 board I2S 参数，也不能要求所有 board 按 20 ms 产出 PCM。Portable Audio integration 负责把连续 PCM stream 切成合法的 Opus frame；例如 16 kHz Opus 的 20 ms frame 是每声道 320 samples，而 Tiga provider 仍可每次交付 512 samples。
- Public request API 固定接受 16 kHz mono S16LE PCM。`$gizclaw/audio/uplink` 在内部按 20 ms 连续切片并编码 Opus；`$gizclaw/audio/downlink` 解码 Opus/PLC，并通过有界 PCM ring 向 App 交付。测试专用的 low-level raw Opus API 不属于产品 App 集成边界。
- `GZC_PROTOCOL_OPUS_PACKET` 的 payload 是原始 Opus packet，不带 firmware-private timestamp header。C SDK 和 PAL provider 负责 media/RTP 映射；App 不调用底层 `peer_send_opus`，也不使用 DataChannel fallback。
- 上行 input stream ID 与服务端产生的下行 response stream ID 不要求相同。`libs/gizclaw` 按 `transcript`、`assistant` label 分别绑定本轮第一个 response-local stream ID，并接受其 `:<suffix>` 子流；后续不匹配的 response ID 作为旧轮事件丢弃。RTP audio 仍由同一个 conversation generation 接收，不以 input stream ID 过滤。
- Capture deadline 由实际 `samples_per_channel / sample_rate_hz` 累加，不用固定 sleep；活跃 media poll 的等待上界不得形成 100 ms 音频空洞。
- PCM uplink/downlink 使用单生产者、单消费者的无锁 byte ring；encoded uplink/downlink 使用无锁 fixed-slot ring。ring 只通过 acquire/release atomic index 发布数据，不持有 service mutex，也不使用 semaphore 唤醒。Audio Task 每 20 ms 尝试消费一帧；`h2_gizclaw_pcm_track_write()` 和内部 slot 写入都不等待。`h2_gizclaw_pcm_track_write()` 成功后调用方可以释放 chunk；`WOULD_BLOCK` 表示本次 chunk 未被接受，实时调用方应丢弃并记录 overrun，不能阻塞 microphone 或积累延迟。`h2_gizclaw_service_audio_end()` 冻结当前已接受的 PCM 前缀并发布 EOS，encoder drain 已接受的 PCM 后补齐最后一个非空残片。
- 下行 PCM 只走 Track，不复制到 callback。每个 reply 交付给 App main loop 的通知数量有界且与 reply 长度无关：至多一次 `REPLY_AUDIO_STARTED`（第一块解码 PCM 写入 Track 之后、该 reply 的任何文本事件和 boundary 之前），文本事件，然后恰好一次 `REPLY_DONE` 或 `ERROR`。没有解码出音频的 reply 不产生 `REPLY_AUDIO_STARTED`；realtime 的每轮 VAD reply 和被 barge-in 打断后的新 reply 各自重新产生。Reply boundary 在 EOS 标记解码后立即 stage，不等待 App 侧的任何 drain。App 用 `REPLY_AUDIO_STARTED` 切换到 REPLYING 状态，用 Track 深度或 speaker pump 判断播放进度。
- Opus encode/decode 属于 `libs/gizclaw`，不进入 board driver。接收 provider 的有界重排与 loss marker 合同保持不变；downlink decoder 对 loss marker 执行 PLC，不能直接删除缺失时间。
- GizClaw service network task 不操作 App state 或 LVGL。App main loop dispatch matching-generation callback 后，才把录音电平、等待和播放状态投影到页面 subject；API completion 不是 Runtime event。

### Speech RPC 音频流

Service-owned Speech Transcribe 与 Speech Extract 走与 Conversation 相同的
Track 路径，没有按 chunk 写音频的函数：`h2_gizclaw_req_create_speech_transcribe()`
/ `h2_gizclaw_req_create_speech_extract()` 只复制 options（其中
`options.content_type` 必须描述绑定 Track 的实际 PCM 格式），
`h2_gizclaw_req_do(request, user, NULL, NULL, on_complete)` 预留 Track 的 uplink
路由，`h2_gizclaw_service_audio_start()` 开始采集，App 通过
`h2_gizclaw_pcm_track_write()` 交付 16 kHz mono S16LE PCM，
`h2_gizclaw_service_audio_end()` 冻结已接受的 PCM、drain 后发送 EOS。SDK 不从函数名
推断 Opus，也不执行编码或转码；同一时刻只有一个 uplink 持有者，冲突的 `req_do`
返回 `BUSY` 而不替换现有持有者。

`req_wait` 本身不会结束录音。Completion 仍只由 App main loop 调用
`h2_gizclaw_service_poll()` 后执行；terminal transcript、result JSON 与 operation
result 由 request handle 持有，通过 `h2_gizclaw_resp_parse_speech_transcribe()` /
`h2_gizclaw_resp_parse_speech_extract()` 解析到调用方 storage，直到
`h2_gizclaw_req_release()`。

## 打断与错误

新的 record action 可以按产品交互模式取消当前回复或结束自然对话，但必须产生显式 cancel command。取消顺序为使 App generation 失效、停止新输入、取消 GizClaw operation、停止/关闭 Audio，并等待 matching completion callback 清理 operation context。Disconnect、mic failure、decode failure、speaker failure 和 timeout 都返回带 generation 的 domain error。

连接失败不应反复打开 microphone；Audio 启动失败也不应销毁仍可复用的 GizClaw connection。重试由 App policy 决定，observer 不自动重试。

## H106 接入

H106 首页的 `record` component action 按本页边界接入。Tiga 的 ADC record 键与 Desktop 的 host key 只负责产生相同 action；两端共用 H106 App 自己持有的 chat state 和 effect。具体交互见 产品对话流程。

## 验收

- 按压式 pressed/released 和自然对话 click 都能形成完整 conversation lifecycle。
- 当前 active workspace 在整个 generation 内保持稳定。
- 同一 GizClaw connection generation 和 Workspace 的连续 conversation 不重复 activate；连接重建或 Workspace 切换后重新确认一次。
- 输入 PCM 由 App-owned Mic Task 写入 GizClaw PCM ring，由 GizClaw uplink Task 每 20 ms 切片、编码并通过 WebRTC audio RTP 上行；ring 满时返回 `WOULD_BLOCK`，App 丢弃当前 realtime chunk 并记录 overrun，不等待或改写 payload。
- Speech Transcribe/Extract 按 `content_type` 接受不超过 1280 bytes 的 audio chunk；测试覆盖 timeout 透传、queue 满背压、audio-before-EOS FIFO，以及 commit/terminal 后拒绝写入。
- 当前已接受 response route 的服务端 EOS 与本地 playback drain 都完成后才进入 idle；Chatroom 可以终止于 transcript route，Agent workflow 可以终止于 assistant route，不能用上行 input stream ID 过滤 response-local terminal。
- Cancel、disconnect 和 Audio failure 都关闭本轮 mic/track，不泄漏 task、queue 或 buffer。
- 后台 Audio callback 不直接更新 LVGL；GizClaw callback 由 App main loop dispatch。
- H2Peer host performance gate 在三条并发 request DataChannel（其中一条执行双向各 1 MiB 传输）以及长期 Packet/Event traffic 期间发送 50 个 20 ms Opus RTP frame，要求 frame 完整、有序、无 submit deadline miss，且相邻到达间隔不超过 40 ms；该 gate 验证 transport coexistence，不替代真实设备声学验收。

## Desktop E2E 边界

手动 GizClaw PAL E2E 在测试 integration 中把固定 16 kHz mono S16LE 合成语音编码成 20 ms raw Opus packet，再经 public conversation API 进入 selected Desktop WebRTC PAL。验收要求同 generation 的非空 text、raw Opus 下行和 reply terminal；测试侧固定 libopus decoder 对 raw packet 解码并确认非静音。PAL audio-decoder contract 当前只支持 AAC，因此该测试不把 Opus 编解码错误地声明为 GizClaw 或 audio-decoder PAL 能力。

terminal 后，测试通过 public Workspace history API 查找本轮发送 Gear 对应的新增 Gear entry，要求 transcript 非空且可回放；再 stream 下载 `audio/ogg`，核对 metadata 与接收长度并独立解析、解码 Ogg/Opus。Chatroom 只做转写和转发，不运行 LLM，也不产生 Agent history。这个 transport gate 不替代 provider 语义质量或真实设备声学验收。

Friend Group 语音仍只通过 Group system Workspace 的 Conversation 写入，不存在 `server.friend_group.messages.send`。读取时以 Friend Group scoped name 调用 message list/get；wrapper 返回稳定 `history_id`，其值逐字节来自底层 wire history name。音频通过 `server.friend_group.messages.audio.get` 接收 metadata、二进制 frames 和唯一 EOS，调用方必须核对声明长度与实际接收长度，并在取消、超限或缺失 EOS 时删除部分文件。Speech transcribe/extract/synthesize 的 RuntimeProfile 投影同样按 Model name 选择，不使用 catalog ID 或 alias。
