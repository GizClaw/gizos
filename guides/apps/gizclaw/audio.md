# GizClaw 与 Audio System

Runtime Audio System 负责设备录音、PCM 数据交付和扬声器播放。App integration 通过 typed GizClaw service operation 协调 Workspace、conversation setup、输入 PCM、回复 event 和 cancel 合同，同时继续拥有 Audio lifecycle；service worker 可以用同步 progress call 请求 main loop 执行一次录音、播放或状态步骤，但不能自行调用 Audio PAL。这些阶段仍是 App-owned state，不是 GizClaw SDK state。

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
    Audio-->>Loop: input frames
    Loop->>Service: submit/maintain conversation operation
    Service->>Giz: send audio stream
    UI->>Loop: CommitTurn / StopConversation
    Loop->>Audio: stop microphone
    Loop->>Service: submit commit/cancel operation
    Service->>Giz: commit input
    Service-->>Loop: dispatch response audio callback
    Loop->>Audio: enqueue playback frames
    Service-->>Loop: dispatch response terminal callback
    Audio-->>Loop: playback drained
    Loop-->>UI: conversation idle
```

按压式交互在 record pressed 后启动录音，released 后停止麦克风并提交 turn。自然对话在首次 click 后保持 session active，由 Audio/VAD 与 GizClaw realtime flow 推进；再次 click 或 Back 产生 cancel/stop command。

## Public API 与连接要求

| 对话集成所需信号 | Audio System 行为 | GizClaw Public API |
| --- | --- | --- |
| Workspace ready | 尚不读取麦克风 | Workspace activate result |
| 输入开始 | 启动 microphone，按帧 deadline 产出 PCM | `h2_gizclaw_conversation_open()` |
| 输入 PCM | 交付 provider 自然块长的完整 S16LE PCM frame | `h2_gizclaw_conversation_configure_pcm()`、`h2_gizclaw_conversation_write_pcm()` |
| 输入提交 | 停止 microphone，并提交本轮输入结束 | `h2_gizclaw_conversation_commit()` |
| 回复 Opus | 解码并写入有界播放 track | `h2_gizclaw_conversation_poll()` 的 reply audio event |
| 回复 terminal | 等待本地 playback drain 后结束本轮 | `h2_gizclaw_conversation_poll()` 的 terminal event |
| Cancel | 停止 mic、关闭输入、释放或 drain 输出 | `h2_gizclaw_conversation_cancel()` |

Conversation 完成同时满足服务端 response terminal 和本地 playback drained。

## Audio 格式与背压

- Browser/opaque-track 模式不经过下面的 PCM/Opus packet API。Runtime 把平台 Track
  传入 `h2_gizclaw_config_t.webrtc_media_track`，GizClaw 只负责 conversation
  BOS/EOS 与控制事件，WebRTC provider 直接拥有 microphone、remote playback、codec
  和 RTP progression。Track 的布局不跨平台共享。
- Audio format 和 provider frame size 由 Runtime Audio capability 决定，App 不能写死 board I2S 参数，也不能要求所有 board 按 20 ms 产出 PCM。
- `libs/gizclaw` 复制并累计 provider 交付的完整 S16LE PCM frame，再按采样率切成 20 ms Opus frame 编码和发送。例如 16 kHz 下 Opus frame 是每声道 320 samples，Tiga provider 仍可保持每次 512 samples；切片边界由连续 PCM stream 决定。调用方通过 `h2_gizclaw_conversation_configure_pcm()` 传入 0 到 10 的 Opus complexity；H106 使用 0。库内固定 4 个 Opus packet 的 transmit ring 只承接 PCM 切片产生的短时突发。
- `h2_gizclaw_conversation_write_opus()` 保留为已经持有 raw Opus packet 的低层入口；同一 conversation 不能混用 PCM 和 raw Opus 输入模式。
- `GZC_PROTOCOL_OPUS_PACKET` 的 payload 是原始 Opus packet，不带 firmware-private timestamp header。C SDK 和 PAL provider 负责 media/RTP 映射；App 不调用底层 `peer_send_opus`，也不使用 DataChannel fallback。
- 上行 input stream ID 与服务端产生的下行 response stream ID 不要求相同。`libs/gizclaw` 按 `transcript`、`assistant` label 分别绑定本轮第一个 response-local stream ID，并接受其 `:<suffix>` 子流；后续不匹配的 response ID 作为旧轮事件丢弃。RTP audio 仍由同一个 conversation generation 接收，不以 input stream ID 过滤。
- Capture deadline 由实际 `samples_per_channel / sample_rate_hz` 累加，不用固定 sleep；活跃 media poll 的等待上界不得形成 100 ms 音频空洞。
- Capture 和 playback 使用有界 buffer。复制成功后的切片和可配置有界 Opus transmit ring 由 `libs/gizclaw` 持有，调用方可以释放原始 PCM。每次编码后立即尝试按序排空 ring；transport 返回 `WOULD_BLOCK` 且 ring 已满时覆盖最旧 Opus packet，继续消费 PCM。`commit` 只把最后一个非空残片补零编码一次，ring 全部发送后才提交 EOS，排空期间仍可返回 `WOULD_BLOCK`，空输入不发送静音 packet。
- Opus encode/decode、resample 或 channel conversion 属于 portable Audio/integration 层，不进入 board driver；当前 PCM uplink 只接受 libopus 原生支持的 sample rate、mono/stereo 和 S16LE，其他转换必须在调用入口前完成。接收 provider 先在 bounded reorder window 内恢复 Opus RTP 乱序；确认 sequence 缺失，或连续 sequence 的 48 kHz RTP timestamp 存在整 20 ms packet 缺口后，才以 `opus == NULL && opus_len == 0` 逐包交付 loss marker。Decoder owner 必须按最近 packet duration 执行 Opus PLC，不能把缺失时间直接删除后拼接后续语音。
- GizClaw service worker 不操作 App state 或 LVGL。App main loop dispatch matching-generation callback 后，才把录音电平、等待和播放状态投影到页面 subject；API completion 不是 Runtime event。

## 打断与错误

新的 record action 可以按产品交互模式取消当前回复或结束自然对话，但必须产生显式 cancel command。取消顺序为使 App generation 失效、停止新输入、取消 GizClaw operation、停止/关闭 Audio，并等待 matching completion callback 清理 operation context。Disconnect、mic failure、decode failure、speaker failure 和 timeout 都返回带 generation 的 domain error。

连接失败不应反复打开 microphone；Audio 启动失败也不应销毁仍可复用的 GizClaw connection。重试由 App policy 决定，observer 不自动重试。

## H106 接入

H106 首页的 `record` component action 按本页边界接入。Tiga 的 ADC record 键与 Desktop 的 host key 只负责产生相同 action；两端共用 H106 App 自己持有的 chat state 和 effect。具体交互见 产品对话流程。

## 验收

- 按压式 pressed/released 和自然对话 click 都能形成完整 conversation lifecycle。
- 当前 active workspace 在整个 generation 内保持稳定。
- 同一 GizClaw connection generation 和 Workspace 的连续 conversation 不重复 activate；连接重建或 Workspace 切换后重新确认一次。
- 输入 PCM 由 `libs/gizclaw` 统一切成 20 ms raw Opus packet，通过 WebRTC audio RTP 上行；`WOULD_BLOCK` 不丢 provider frame、不改写 payload。
- 当前已接受 response route 的服务端 EOS 与本地 playback drain 都完成后才进入 idle；Chatroom 可以终止于 transcript route，Agent workflow 可以终止于 assistant route，不能用上行 input stream ID 过滤 response-local terminal。
- Cancel、disconnect 和 Audio failure 都关闭本轮 mic/track，不泄漏 task、queue 或 buffer。
- 后台 Audio/GizClaw callback 不直接更新 LVGL。
- H2Peer host performance gate 在三条并发 request DataChannel（其中一条执行双向各 1 MiB 传输）以及长期 Packet/Event traffic 期间发送 50 个 20 ms Opus RTP frame，要求 frame 完整、有序、无 submit deadline miss，且相邻到达间隔不超过 40 ms；该 gate 验证 transport coexistence，不替代真实设备声学验收。

## Desktop E2E 边界

手动 GizClaw PAL E2E 使用固定 16 kHz mono S16LE 合成语音，经 public conversation API 进入 selected Desktop WebRTC PAL。验收要求同 generation 的非空 text、raw Opus 下行和 reply terminal；测试侧固定 libopus decoder 对 raw packet 解码并确认非静音。PAL audio-decoder contract 当前只支持 AAC，因此该测试不把 Opus 解码错误地声明为 audio-decoder PAL 能力。

terminal 后，测试通过 public Workspace history API 查找本轮发送 Gear 对应的新增 Gear entry，要求 transcript 非空且可回放；再 stream 下载 `audio/ogg`，核对 metadata 与接收长度并独立解析、解码 Ogg/Opus。Chatroom 只做转写和转发，不运行 LLM，也不产生 Agent history。这个 transport gate 不替代 provider 语义质量或真实设备声学验收。

Friend Group 语音仍只通过 Group system Workspace 的 Conversation 写入，不存在 `server.friend_group.messages.send`。读取时以 Friend Group scoped name 调用 message list/get；wrapper 返回稳定 `history_id`，其值逐字节来自底层 wire history name。音频通过 `server.friend_group.messages.audio.get` 接收 metadata、二进制 frames 和唯一 EOS，调用方必须核对声明长度与实际接收长度，并在取消、超限或缺失 EOS 时删除部分文件。Speech transcribe/extract/synthesize 的 RuntimeProfile 投影同样按 Model name 选择，不使用 catalog ID 或 alias。
