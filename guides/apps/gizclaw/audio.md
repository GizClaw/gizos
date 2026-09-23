# GizClaw 与 Audio System

Runtime Audio System 负责设备录音和扬声器 Track；GizClaw service 负责 Conversation 的 PCM 缓冲、Opus 编解码、BOS/EOS 和网络媒体状态。App integration 只在独立 Audio Task 中把 microphone PCM 写入 GizClaw，并把 GizClaw 回调交付的 PCM 写入播放 Track。Service callback 仍经 App main loop dispatch，不能直接修改 LVGL。

Peer connection 内的上下行 Opus RTP track、双向 Agent Event Stream、BOS/EOS 与动态 RPC DataChannel 的关系见 [GizClaw Peer Connection 传输拓扑](/apps/gizclaw/transport)。

## 本地在线音乐播放器

`libs/gizclaw` 的本地播放器使用库内的 Ogg/Opus decoder，解码后交给 config 注入的 Audio PAL 播放 Track。它不调用 PAL audio-decoder，也不受该独立 capability 当前仅支持 AAC 的限制。播放器与 Conversation RTP 是不同入口；App 可调用 `h2_gizclaw_player_play()`、`play_index()`、`play_index_at()`、`playlist_set()`、`repeat_set()`、`stop()`、`get_status()`、`playlist_snapshot()`，服务器反向 audio player RPC 复用相同 worker、状态与 telemetry。

设备自身持有 playlist，服务器 `playlist.set` / `append` 推送的专辑不经过网络回读即可展示。`h2_gizclaw_player_playlist_snapshot()` 在设备锁下整份拷贝出 caller-owned 快照：最多 `H2_GIZCLAW_PLAYER_PLAYLIST_MAX_ITEMS`（32）个条目的 title 与 source_ref、条目数、current index 及其存在标志、playlist revision 和 repeat 模式。快照不含每条最长 1 KB 的 URL，只有库自己需要它下载；UI 靠 revision 判断缓存是否失效，未变化就不重绘。`h2_gizclaw_player_get_status()` 另外附带 `has_current_index` / `current_index` / `playlist_length` / `playlist_revision`，使“第 3/8 首”这类投影不必再取一次快照。用户选中某条时调用 `h2_gizclaw_player_play_index()`，它走 `client.device.audioplayer.play` 的同一条内部路径；索引达到或超过 playlist 长度返回 `H2_PAL_ERR_INVALID_ARG`，在改动任何状态之前拒绝，不打断正在播放的曲目。这两个入口都不发起网络请求。

设备自己也能写 playlist：`h2_gizclaw_player_playlist_set()` 接收 caller-owned 的 `h2_gizclaw_player_playlist_entry_t` 数组（每条一个必填的 HTTPS Ogg/Opus URL span，title 与 source_ref 可选，len 为 0 表示不存在；可选 `duration_ms` 是产品已知的曲目时长，0 表示未知）与条目数，走 `client.device.audioplayer.playlist.set` 的同一条内部路径：同样先校验全部 URL 再整体替换，因此失败时上一份 playlist 与正在播放的曲目都不受影响，revision 只在成功时前进。count 为 0 清空 playlist；超过 `H2_GIZCLAW_PLAYER_PLAYLIST_MAX_ITEMS`（32）返回 `H2_PAL_ERR_INVALID_ARG`（不是 `BUSY`，因为再等也放不下），服务停止中返回 `H2_PAL_ERR_CLOSED`。它与 RPC 一样是纯写入，不启动播放，也清掉 current index；要让专辑开始播放，紧接着调用 `h2_gizclaw_player_play_index(service, 0)`。这样“用户选中本地专辑”与“手机推送 playlist”在设备内是同一条路径。

从中途开始播放（例如续播上次听到的长节目）调用 `h2_gizclaw_player_play_index_at(service, index, start_ms)`；`start_ms` 为 0 与 `play_index()` 完全相同。它依赖该条目在 `playlist_set()` 中给出的 `duration_ms`：时长未知（包括服务器推送的条目，wire item 没有时长）时从 0 播放并报告 0；`start_ms` 不小于已知时长返回 `H2_PAL_ERR_INVALID_ARG`，与越界索引一样在改动任何状态前拒绝。时长已知时，库先用 `Range: bytes=0-` 读文件头，解析完 `OpusHead` / `OpusTags` 就取消该请求（文件头多大都行，包括内嵌大封面），再按字节率从起点前约 5 秒处发起 `Range: bytes=<offset>-`，在下一个 CRC 正确的 Ogg page 上重新同步，跳过其余数据直到起点。`status.position_ms` 在缓冲期间显示请求的 `start_ms`，定位后改为由 Ogg granule 精确算出的位置，不是字节估算。服务器忽略 Range（返回 200 全文件）时直接在该响应上跳到起点；`Content-Range` 不符、续传请求不是 206、落点之后没有可用 page 等任何在首个样本前的失败，都会改用一次普通 GET 并跳到起点。起点超过文件实际结尾时，条目在结尾处正常结束并报告真实时长。只有被选中的条目从中途开始，单曲循环和自动下一首都从 0 开始。浏览器构建只有在服务器通过 CORS 暴露 `Content-Range` 时才走 Range 路径，否则走普通 GET 回退。

`h2_gizclaw_player_repeat_set()` 接受与 `client.device.audioplayer.mode.set` 相同的 `off` / `one` / `all`，其他值返回 `H2_PAL_ERR_INVALID_ARG` 并保持当前模式不变，快照的 `repeat` 字段即为回读入口。末曲推进与循环由 library 的 worker 按该模式负责：`one` 重播当前曲，`all` 到末尾回到第 0 条，`off` 播完即停。产品必须设置模式而不是自己实现“下一首、到末尾回绕”，否则会与库内推进重复触发。

### 变速播放（保持音调）

`h2_gizclaw_player_rate_set(service, rate_permille)` 让本地播放器以录制速度的 `rate_permille`‰ 播放，范围 `H2_GIZCLAW_PLAYER_RATE_MIN`（500）到 `H2_GIZCLAW_PLAYER_RATE_MAX`（2000），`H2_GIZCLAW_PLAYER_RATE_NORMAL`（1000）为原速；越界返回 `H2_PAL_ERR_INVALID_ARG` 且速率不变，`get_status()` 的 `rate_permille` 读回当前值。它面向播客、有声书这类整段下载的语音：变速在设备上做，改的是语速不是音高。实时下行的对话回复不走这里——设备放慢实时流会越积越多，那类语速由服务端合成时决定。

速率是播放器属性而不是单次播放参数：设置后在一个 32 ms 步长内作用于正在播放的条目（不重启下载、不重新定位），并对之后的每一条、自动下一首、单曲循环和服务器发起的 `audioplayer.play` 持续生效，直到再次设置。这样产品在用户改偏好时设一次即可；原速播放的内容（如音乐）在开始前设回 1000。命名音效（`client.device.sound.play`）始终原速。

时间轴一律按媒体本身：`status.position_ms`、结束时的 `duration_ms`、`play_index_at()` 的 `start_ms` 和 `playlist_set()` 的 `duration_ms` 都是原始媒体时间，放慢不会改变续播点或上报的时长；变速中途切换时位置只增不减。

实现是定点 SOLA：40 ms 序列、8 ms 线性交叉淡化、在 15 ms 窗口内按绝对差之和找与上一段尾部最相似的起点，每步输出 32 ms、按速率推进源位置。1000 为直通，不分配缓冲、不增加逐样本计算，输出与不变速时逐字节相同；其他速率在条目播放期间占用一块约 5.5 KiB 的固定工作缓冲，条目结束即释放。下载环形缓冲不变——慢放只是消费更慢，已有背压让下载等待更久，不会多预取或涨内存。工作缓冲分配失败时该条目按原速继续播放并记 WARN `player-rate fallback=1`，不会让播放失败；这也是 CPU 不足时的降级方向。每个变速过的条目结束时记一行 INFO `player-rate rate=… audio_ms=… decode_us=… stretch_us=…`，用于在真实板子上核对解码与变速各占多少 CPU。

HTTPS 下载由独立 PAL task 写入有界压缩环形缓冲，默认容量 64 KiB、启动与补缓冲阈值 16 KiB。设备 worker 增量读取 Ogg page 和 Opus packet，输出 16 kHz mono S16LE，并组装成 Audio PAL 要求的完整 PCM frame。下载侧缓冲满时等待，播放侧 `WOULD_BLOCK` 重试同一帧；不会把整首文件载入内存，也不会逐帧 drain 插入静音。只有尾帧补零，正常结束时 drain；播放进度扣除排队帧，最终不计补零样本。

播放下载已收到正文后若连接断开或 15 秒无进度，且响应给出完整文件长度并证明支持字节范围（`206` / `Content-Range` 或 `Accept-Ranges: bytes`），下载 task 最多续传三次。每次从已接收的文件绝对偏移请求 `Range: bytes=<offset>-`，要求 `206`、匹配的 `Content-Range` 和剩余长度；数据继续写入同一个环形缓冲，decoder 与 Track 不重启。重试间隔递增，并响应 stop 或 generation 变化。首字节前失败、范围不被支持、响应长度或范围不符、重试耗尽时仍进入原有错误路径；seek 的定位回退和命名音效的播放语义不变。

停止使当前 generation 失效并取消 HTTP，下载 task join 成功后才释放其缓冲；播放器只关闭自己的 Track，不关闭共享扬声器。下载、解码或输出失败进入 error 并上报 telemetry。命名音效由补充 vtable 解析名称为 HTTPS Ogg/Opus URL；名称须适合内部有界存储，非法输入在预留任务前拒绝。

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
    Service-->>Loop: dispatch completion (input sent)
    Giz-->>Service: Opus RTP, whenever the server speaks
    Service->>Audio: decode PCM into the downlink Track
    Audio-->>Audio: speaker pump reads the Track
    Loop-->>UI: sound playing / idle
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
| 下行 PCM | 扬声器 pump 从有界播放 track 读取，有数据就播 | Track 的 downlink：`h2_gizclaw_pcm_track_read()`；PCM 不经 callback 投递，也没有回复开始或结束事件 |
| 本轮完成 | 输入结束已发出即完成，不等待服务端 | `h2_gizclaw_service_poll()` 分发的 completion callback |
| Cancel / 挂断 | 停止 mic、关闭输入、丢弃未播放输出 | `h2_gizclaw_conversation_cancel()`，之后 `h2_gizclaw_conversation_release()` |

Conversation completion 表示本轮输入已发送，不等待服务端回复或本地播放排空。

## Audio 格式与背压

- Browser/opaque-track 模式不经过下面的 PCM/Opus packet API。Runtime 把平台 Track
  传入 `h2_gizclaw_config_t.webrtc_media_track`，GizClaw 只负责 conversation
  BOS/EOS 与控制事件，WebRTC provider 直接拥有 microphone、remote playback、codec
  和 RTP progression。Track 的布局不跨平台共享。
- Audio format 和 provider frame size 由 Runtime Audio capability 决定，App 不能写死 board I2S 参数，也不能要求所有 board 按 20 ms 产出 PCM。Portable Audio integration 负责把连续 PCM stream 切成合法的 Opus frame；例如 16 kHz Opus 的 20 ms frame 是每声道 320 samples，而 Tiga provider 仍可每次交付 512 samples。
- Public request API 固定接受 16 kHz mono S16LE PCM。`$gizclaw/audio/uplink` 在内部按 20 ms 连续切片并编码 Opus；`$gizclaw/audio/downlink` 解码 Opus/PLC，并通过有界 PCM ring 向 App 交付。测试专用的 low-level raw Opus API 不属于产品 App 集成边界。
- `GZC_PROTOCOL_OPUS_PACKET` 的 payload 是原始 Opus packet，不带 firmware-private timestamp header。C SDK 和 PAL provider 负责 media/RTP 映射；App 不调用底层 `peer_send_opus`，也不使用 DataChannel fallback。
- 上行 input stream ID 只描述我们自己的输入：BOS、READY、EOS，以及服务端对它的拒绝（该 ID 上带我们 label 或无 label 的 EOS error）。下行音频属于连接，不属于任何一轮输入；下行 stream ID 和 EOS 不参与本轮输入判定，音频 BOS 只用于解除下述 PTT hold。未被 hold 或其他 Track owner 抑制的音频解码进 Track 播放，下行流结束（有无错误码）都不是 conversation 错误。文字事件在输入活跃期间转发，不参与状态。
- Capture deadline 由实际 `samples_per_channel / sample_rate_hz` 累加，不用固定 sleep；活跃 media poll 的等待上界不得形成 100 ms 音频空洞。
- PCM uplink/downlink 使用单生产者、单消费者的无锁 byte ring；encoded uplink/downlink 使用无锁 fixed-slot ring。ring 只通过 acquire/release atomic index 发布数据，不持有 service mutex，也不使用 semaphore 唤醒。Audio Task 每 20 ms 尝试消费一帧；`h2_gizclaw_pcm_track_write()` 和内部 slot 写入都不等待。`h2_gizclaw_pcm_track_write()` 成功后调用方可以释放 chunk；`WOULD_BLOCK` 表示本次 chunk 未被接受，实时调用方应丢弃并记录 overrun，不能阻塞 microphone 或积累延迟。`h2_gizclaw_service_audio_end()` 冻结当前已接受的 PCM 前缀并发布 EOS，encoder drain 已接受的 PCM 后补齐最后一个非空残片。
- 下行解码通道在第一个 Conversation 创建时建立，随 Service 存在到 deinit，不随每轮输入或 Conversation release 销毁：产品每轮创建并释放 Conversation，之后到达的音频照常播放。音频播放或 Speech 占用 Track 下行时，到达的对话音频直接丢弃，之前已排队的也丢弃，Track 空出后不会补播旧音频。下行 Opus ring 为 32 格（约 640 ms），满时拒收并由 provider 丢包，不做 PLC、不报错，扬声器卡住时丢音频而不是累积延迟。下行 PCM 只走 Track，不复制到 callback；App 按 speaker pump 实际播放判断“有声音”。
- PTT 松手（真正结束输入的那次成功 `audio_end`，重复调用不算）先清空此刻缓冲的下行音频：待解码 Opus、解码器状态和 Track 里未播的 PCM，再清除 `waiting_for_bos`；即使没有收到下行音频 BOS，之后到达的音频也照常播放。按下置位和松手清空、解除抑制都在同一个 audio control mutex 内串行完成，旧松手不能清掉后一次按下的新 hold。挂断和 Workspace 切换同样清空缓冲；新输入替换旧输入不清空。这里的清空只覆盖本地已缓冲的数据，不为不同 transport 中仍在途的 packet 增加轮次归属或排序保证。
- PTT 按下（`audio_start`，短按打断也算）置位 `waiting_for_bos`：此后到达的下行 Opus 直接丢弃，不进入 ring；按下之后收到的第一个下行音频 BOS（`kind=AUDIO`）或真正松手都可以清除标志，后一次按下会重新置位。文本、转写和我们自己输入的 BOS 不清除标志，EOS 不参与，也不看 stream ID；不使用超时解除抑制。Service 网络任务每一轮都读空 Event stream（与是否有请求在运行、产品是否订阅事件无关），下行 BOS 与下行音频同步处理，不会积压到下一次输入。
- Opus encode/decode 属于 `libs/gizclaw`，不进入 board driver。接收 provider 的有界重排与 loss marker 合同保持不变；downlink decoder 对 loss marker 执行 PLC，不能直接删除缺失时间。
- GizClaw service network task 不操作 App state 或 LVGL。App main loop dispatch matching-generation callback 后，才把录音电平、等待和播放状态投影到页面 subject；API completion 不是 Runtime event。

### Conversation 输入边界

开始输入先发送新 StreamID 的纯控制 BOS（kind 未指定，mime_type 为空），因此上游可以立即打断旧回复。第一块 PCM 到达后才发送同一 StreamID 的音频 BOS，并等待 AUDIO_INPUT_READY 后发送 Opus；结束时先发送已打开音频通道的 EOS，再发送纯控制 EOS。没有 PCM 的输入只发送纯控制 BOS/EOS，不等待音频 READY，也不生成静音包或空文本。

### 完整文字输入

`h2_gizclaw_session_send_text()` 在 Session 当前 Workspace 的空闲路由上复制并异步提交
1–4096 字节 UTF-8 文本，无需启动麦克风。发送纯控制 BOS 后，以同一 stream ID 和
输入 label 发送 TEXT_DONE；TEXT_DONE 自带输入结束，不额外发送 EOS。
录音中或上一输入尚未 completion 时返回 BUSY，不打断录音；受理后 Session 进入
WAITING。完成和错误由原有 poll/completion 分发；完成只表示输入已发送，之后到达的
下行音频照常播放。详细错误、Session 状态与取消规则见
[Conversation 文字输入](/zh/developing/gizclaw#conversation-文字输入)。

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

## PTT 控制与输入失败诊断

控制日志通过 Session generation 和 request identity/generation 关联；状态在锁内
记录、解锁后输出。幂等跳过仅记 DEBUG，成功返回不一定意味着执行了新的 input end。
`committed` 表示本地输入封口，`media_eos` 表示媒体消费完毕，
`transport_committed` 表示协议 input end 已发送。

取消记录本轮首次 `cancel_source`：0 未指定、1 public cancel API、2 Session 重启输入、
3 Workspace 切换、4 realtime input end。`cancel_requested` 的返回码表示调用是否成功，
网络 owner 观察到取消时记录 INFO `cancel_state`。App 仍需记录用户挂断、麦克风失败等
具体触发原因；来源 1 本身不能区分这些原因。

音频 worker 仅在本轮首个致命错误时记录 ERROR `audio_worker_failed`，包含失败阶段、
PAL 返回码、identity/generation、输入状态及 PCM/Opus 计数。成功、WOULD_BLOCK 和常规
TIMEOUT 不增加逐帧日志。网络 owner 随后记录 `audio_failed`；控制 BOS、音频 READY
超时和初始时钟失败也保留对应阶段日志。日志不包含音频内容、转写或回复文本。

## H106 接入

H106 首页的 `record` component action 按本页边界接入。Tiga 的 ADC record 键与 Desktop 的 host key 只负责产生相同 action；两端共用 H106 App 自己持有的 chat state 和 effect。具体交互见 产品对话流程。

## 验收

- 按压式 pressed/released 和自然对话 click 都能形成完整 conversation lifecycle。
- 当前 active workspace 在整个 generation 内保持稳定。
- 同一 GizClaw connection generation 和 Workspace 的连续 conversation 不重复 activate；连接重建或 Workspace 切换后重新确认一次。
- 输入 PCM 由 App-owned Mic Task 写入 GizClaw PCM ring，由 GizClaw uplink Task 每 20 ms 切片、编码并通过 WebRTC audio RTP 上行；ring 满时返回 `WOULD_BLOCK`，App 丢弃当前 realtime chunk 并记录 overrun，不等待或改写 payload。
- Speech Transcribe/Extract 按 `content_type` 接受不超过 1280 bytes 的 audio chunk；测试覆盖 timeout 透传、queue 满背压、audio-before-EOS FIFO，以及 commit/terminal 后拒绝写入。
- PTT 一轮在输入结束发出后完成，不等待服务端；翻译、flowcraft 等多段回复都只是之后到达的下行音频，照常播放。Realtime 一轮持续到挂断。下行事件只以 DEBUG 记录。Friend / Friend Group 的 SFU Workspace 不给发言者任何下行音频，发送后本轮正常完成。
- Cancel、disconnect 和 Audio failure 都关闭本轮 mic/track，不泄漏 task、queue 或 buffer。
- 后台 Audio callback 不直接更新 LVGL；GizClaw callback 由 App main loop dispatch。
- H2Peer host performance gate 在三条并发 request DataChannel（其中一条执行双向各 1 MiB 传输）以及长期 Packet/Event traffic 期间发送 50 个 20 ms Opus RTP frame，要求 frame 完整、有序、无 submit deadline miss，且相邻到达间隔不超过 40 ms；该 gate 验证 transport coexistence，不替代真实设备声学验收。

## Desktop E2E 边界

手动 GizClaw PAL E2E 在测试 integration 中把固定 16 kHz mono S16LE 合成语音编码成 20 ms raw Opus packet，再经 public conversation API 进入 selected Desktop WebRTC PAL。验收以 speaker pump 实际听到的非静音下行为准：听到声音之后安静 300 ms 计为一轮，PTT 在本轮完成后继续播放直到听到回复；测试侧固定 libopus decoder 对 raw packet 解码并确认非静音。PAL audio-decoder contract 当前只支持 AAC，因此该 Conversation 测试不通过 audio-decoder PAL 编解码 Opus；本页本地音乐播放器的库内 Ogg/Opus decoder 是独立能力。

terminal 后，测试通过 public Workspace history API 查找本轮发送 Gear 对应的新增 Gear entry，要求 transcript 非空且可回放；再 stream 下载 `audio/ogg`，核对 metadata 与接收长度并独立解析、解码 Ogg/Opus。这个 transport gate 不替代 provider 语义质量或真实设备声学验收。

Friend 与 Friend Group 语音只通过各自 system Workspace（内置 `system-sfu` Workflow）的 Conversation 写入。GizClaw 0.15 起该 Workspace 是 LiveKit SFU 的单工对讲：Server 侧 connector 代表 Peer 入房，Device 保持原有 WebRTC 连接、不感知 LiveKit；下行是锁定一路发言者的 Opus 原样透传，发言者收不到自己的下行。SFU Workspace 没有 History、消息或音频资产，`server.friend_group.messages.*` 已删除，libs/gizclaw 不再提供 friend group message list/get/audio download。发送期间被拒绝的输入以同一 `stream_id` 的 typed EOS error 返回（`SFU_RUNTIME_NOT_ATTACHED`、`SFU_ACCESS_REVOKED`、`SFU_ACCESS_CHECK_FAILED`），App 按 code 结束本轮录音状态，不自动切换 Workspace。Speech transcribe/extract/synthesize 的 RuntimeProfile 投影同样按 Model name 选择，不使用 catalog ID 或 alias。

### 下行边界与取消

库内诊断通过 `h2_gizclaw_conversation_downlink_counters_internal(service)` 一次取得 `h2_gizclaw_conversation_downlink_counters_t` 快照：`received` 是 downlink 存在期间收到的合法 Opus 写入次数（包含零长度 PLC 标记），`dropped_no_track` 是 audio play 或 Speech 占用 Track 时的丢弃数，`dropped_waiting_for_bos` 是 hold 尚未解除时的丢弃数，`dropped_ring_full` 是 Opus ring 返回 `WOULD_BLOCK` 时的丢弃数。按现有入口判断顺序归因：Track 占用优先于 hold，一次写入最多增加一种丢弃原因。读取时只获取一次 downlink 引用，Track 被占用时仍可读取；这属于 private internal 诊断接口，不是 App public API。

四个计数随 downlink 创建归零、随 Service deinit 销毁，hold、BOS、flush 和 Conversation release 都不重置；使用 `atomic_uint_least32_t`，按 `uint_least32_t` 的位宽无符号回绕。各字段单独原子读取，并发写入时快照不保证字段间处于同一时刻；诊断可比较同一 downlink 生命周期内相邻快照的增量。对象尚未创建或 service 为 NULL 时读取全零，无对象时的到达和非法参数不计数。计数只观察既有接收路径：no-track 和 waiting-for-bos 仍返回 OK，ring-full 仍返回 `WOULD_BLOCK`，CLOSED 仍映射为 OK 且不计入 ring-full；后续 flush、解码失败及实际扬声器播放量不在这三个丢弃计数内，不能用它们推断已经听到了回复。

下行媒体不看 EOS，除按下后的 `waiting_for_bos` 外收到即解码；标志只由按下后的下一个下行音频 BOS 清除。清空与 decoder 写入 Track 串行化：清空时持有解码锁，丢弃待解码 Opus、重置解码器并标记 Track 下行水位，旧数据不会在清空后再写入。已交给平台输出的音频缓冲不在此清空保证内。不新增 RTP payload 或时间戳格式。

`h2_gizclaw_conversation_cancel` 只关闭本轮输入并清空本地缓冲，不停止服务端 run；SFU 房间下行可继续到达。显式离开当前 run 使用 `h2_gizclaw_rpc_run_stop`，服务端停止 runtime 并断开房间，成功后库再清空 Service 的 Opus 队列、解码器和未播放 PCM（无 Session 时也一样）。下行仍由 Service 共享 Track 承接，不按 stream 或 Workspace 过滤；已交给平台输出的缓冲仍不在清空保证内。
