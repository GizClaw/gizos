# H2Peer

H2Peer 是 `libs/pal/providers/h2peer` 中由 GizOS 维护的 portable WebRTC core。它实现 `h2_pal_webrtc_api_t`，但不拥有产品 signaling、codec、音频设备或 target wiring。现有 target 只有在各自 component 显式选择并提供经过验证的安全 provider 后，才会使用 H2Peer。

## Ownership

```text
libs/pal/providers/h2peer/
├── include/h2_peer.h                    # public instance lifecycle
├── src/h2_peer_webrtc.c                 # WebRTC PAL state and ownership
├── src/ice/                             # ICE server validation
├── src/sdp/                             # bounded SDP codec
├── src/stun/                            # bounded STUN codec
├── src/media/                           # RTP and RTCP wire codec
├── src/data_channel/                    # DCEP wire codec
├── src/turn/                            # TURN refresh scheduling
├── src/providers/                       # private provider contracts and portable backend
└── tests/                               # host tests and fake providers
```

Public consumer 只 include `h2_peer.h`。`src/` 中的 provider type、session、parser state 和 test injection entry 都是 package-private implementation，不能成为 app、board 或 target component 的 public contract。Vendor adapter 可以实现这些窄接口，但 vendor header 和 handle 必须继续留在 H2Peer private source 或 target component private source。

## Lifecycle

每个 `h2_peer_t` 借用调用方提供的 PAL mem、log、net、queue、sync、task、time、crypto、DTLS 和 SCTP API object，并拥有一个 `h2_pal_webrtc_api_t` view。Log API 必须提供可调用的 `write`；H2Peer 的协议诊断统一带 `h2peer` scope 写入这个 instance-owned PAL capability，不读取或写入 `stdout`、`stderr` 或 newlib reentrancy state。该 view 的 `user` 指向对应 H2Peer instance；仓库不保存 process-global current instance。

调用方必须保证所有 PAL API object 及其 backend state 存活到 `h2_peer_destroy()` 返回。销毁 H2Peer 会先关闭仍存活的 peer、channel 和 private provider session，再释放 package-owned string 和 state；不会销毁借用的 PAL backend。正常 `h2_peer_create()` 要求完整的 PAL UDP、bounded TCP client、毫秒与微秒 monotonic time、sleep、memory、sync、task、crypto、DTLS 和 SCTP 操作；缺失操作在构造时明确返回 `H2_PAL_ERR_UNSUPPORTED`，不会选择 test fake 或 plaintext fallback。

每个 WebRTC peer 独立拥有 ICE server copy、connection state、RTP sequence/timestamp、DataChannel stream identity 和 provider session。开始 offer 后不能继续添加 ICE server。SDP callback buffer 和收到的 Opus/DataChannel buffer 只在同步 callback 期间有效。

H2Peer 的本地 DataChannel SID pool 固定为 DTLS client parity 的 150 个 odd SID（`1..299`）。自动创建会从该 pool 扫描可用 SID；所有 live channel 或 reset quarantine 都占用对应 entry，全部占满时稳定返回 `H2_PAL_ERR_NO_SPACE`。显式 SID 必须满足本地 parity、范围且当前可用，否则返回 `H2_PAL_ERR_INVALID_ARG`。尚未成功提交 DCEP 的 channel 关闭后立即回收；已经上 wire 的 channel 必须收到 RFC 6525 outgoing-reset completion 和 peer incoming-reset 两个方向的完成证据，删除旧 stream mapping 后才可复用。一次 peer 只提交一个 reset request，其余关闭排队；`BUSY` 和 `WOULD_BLOCK` 在后续 poll 重试，其他 reset failure 使剩余 channel 进入 `ERROR`、peer 进入 `FAILED`。

`h2_pal_webrtc_channel_close()` 消费 channel handle；调用返回后不能再次发送、关闭或保存该 handle。`CLOSED`/`ERROR` callback 中的 channel 和 info 只在同步 callback 期间作为终态 borrowed view 有效，callback 返回后同样失效。Peer close 或 transport terminal event 会释放所有仍存活 channel 及其 label storage。

## Network Task And Dispatch

每个 production WebRTC peer 创建一个名为 `h2peer/net` 的 protocol owner task。只有这个 task 可以读写 ICE、DTLS、SRTP、SCTP、RTP 和 DataChannel connection state；App、Runtime、GizClaw 或测试 task 不能直接驱动 socket 或协议状态。Direct selected UDP path 也由 owner 负责读写，socket 不会跨 task 竞争。Owner 每轮用 timeout 0 逐包读取并立即推进协议，最多连续处理 16 个 datagram；遇到 `WOULD_BLOCK` 后再处理用户侧发送。TCP、TURN 和尚未完成 ICE selection 的连接继续走同一个 owner task。ESP 和 BK target 对 owner 使用 PSRAM stack policy，owner 请求 `32 KiB` stack。

控制面使用一项 command queue、一项 response queue和单一 request mutex。Open、close、offer 和 remote SDP 等 public control call 同步 marshal 到 owner task。DataChannel send 不进入控制 queue：每个 channel 使用一个 slot，调用方把完整 message 复制到空 slot 后返回；slot 被占用时返回 `H2_PAL_ERR_WOULD_BLOCK` 且零字节消费。Opus 使用一个独立 slot，避免音频被 DataChannel upload 排在后面。Owner 用一个 32-bit atomic ready set 调度最多 32 条同时存活的 production channel；创建第 33 条时稳定返回 `H2_PAL_ERR_NO_SPACE`。Owner 只为 DataChannel 查询 SCTP association 整体是否可写；不可写时保留本地 ready snapshot，只推进 UDP、ACK、timer 和独立的 RTP/SRTP slot。RTP 不能被 SCTP congestion gate 暂停，自身的 DTLS/SRTP transmit backpressure 通过该 slot 的 `WOULD_BLOCK` 保留。SCTP 可写时 owner 用 atomic exchange 取得全部新 ready bits，与尚未发送完的本地 snapshot 分轮处理；任一 DataChannel send 返回 `WOULD_BLOCK` 时保留当前及其余 bits，恢复可写后继续。Slot buffer 按该 channel 已提交的最大 message 扩容并复用，到 channel close 才释放。

返回方向兼容 callback 与显式 pull receive 两种模式。既有 `peer_create()` 保持 callback 行为；pull caller 使用 `peer_create_pull()` 选择 DataChannel、Opus 或两者。每个 DataChannel 使用自己的四槽 receive mailbox 和单槽 gate，Opus 使用独立的四槽 mailbox；`channel_receive()` 或 `peer_receive_opus()` 可以按 timeout 阻塞调用方 task，但不会阻塞 protocol owner。一个 mailbox 填满时暂停 transport receive，任一 slot 被 caller 取走后通过合并 wakeup 恢复；buffer 太小时保留原 message 并返回所需长度。Peer event queue 还承载一个合并的 receive-ready 通知，使同时管理多个 channel 的 caller 可以用 `peer_poll()` 等待后再逐 channel non-blocking receive。Callback event queue 保存指针，事件及 payload 按实际发生数量分配，避免为空闲 peer 预分配 300 多个完整事件结构。

DataChannel 或 RTP TX slot 被 protocol owner 释放时，owner 在同一 event queue 投递合并的 send-ready 通知。该通知没有 public callback payload；它会立即唤醒使用正 timeout 等待的 `peer_poll()`，由调用方重试此前未被消费的完整 message 或 frame。Production H2Peer 的 event queue 等待超时映射为 `H2_PAL_OK`，不会从正 timeout 的 `peer_poll()` 返回 `H2_PAL_ERR_WOULD_BLOCK`。

Callback receive 的正常 high-water 是 12 项或 `16 KiB` queued payload；pull receive 的全局 high-water 是 12 项，并受每个 mailbox 的四项容量进一步限制。达到 high-water 后只暂停 transport receive，control 和待发送工作继续处理。 Opus pull mailbox 是例外：四个 slot 都被占用时新到的 Opus frame 直接丢弃，由消费端的 sequence gap 与 loss marker 处理，不暂停 transport receive，也不把 `H2_PAL_ERR_FULL` 记为 terminal transport error；实时音频的消费者落后超过四帧时丢帧比中断连接正确。Callback payload 复制为 event-owned storage，pull payload 复制到对应 mailbox 的可复用 storage。单个最大 `64 KiB` message 可以使瞬时 queued payload 高于 byte high-water，但不会预分配 receive window 大小。Allocation 或 queue capacity 耗尽会记录为 terminal transport error，不会让 owner task 阻塞等待调用者排空同一队列。

固定 payload storage 包括一个约 `1275 B` 的 RTP TX slot；UDP receive 使用 owner task stack 上的 `1500 B` 临时 buffer，不跨 task 复制或预留 UDP mailbox。进入 protocol owner 的 DataChannel TX 和 RTP TX 各只允许一个待处理 payload；离开 protocol owner 的每个 DataChannel pull RX 和 Opus pull RX 各保留四个 slot，用额外 slot 吸收下游 task 的短暂调度抖动。DataChannel storage 按需扩容并复用；以性能测试使用的 `8 KiB` App chunk 为例，一个同时收发的 channel 峰值是 `8 KiB` TX 加 `32 KiB` RX。Task stack、queue item 和 mailbox buffer 都使用 H2Peer 注入的 Memory PAL；ESP production wiring 可以把这些大块 allocation 放到 PSRAM，FreeRTOS queue、task 和 mutex control block 仍由平台决定。该内存模型需要在真实 target 上用 heap telemetry 验证，不能只用 Desktop throughput 推断。

Worker task 不直接执行 App callback。`peer_start_offer()` 和 `channel_close()` 为保持既有同步时序，在命令完成后由调用者 task 立即派发对应控制 event；持续连接状态、DataChannel message 和 Opus frame 由调用者执行 `peer_poll()` 时派发。Callback 可以安全地重新调用 send、close 或其他 WebRTC API；send 只提交 mailbox，control call 重新进入 command queue，不会重入协议内核。

## Production Provider Flow

正常构造选择 package-private portable backend。ICE 保留 UDP host、server-reflexive 和 relay candidate，并为每个 host address 广告 port `9` 的 active TCP host candidate。Remote UDP 与 passive TCP candidate 进入同一 checklist；pair 只匹配相同 component、address family 和 transport，TCP 还要求 local active 与 remote passive。Checklist 按 priority 稳定降序检查，使同 candidate class 的 UDP 优先；未选中的 TCP connect、check 或 nomination 失败只关闭该 pair 的 connection并继续下一 pair，全部合法 pair 失败才终止 peer。

Selected UDP pair 继续使用 datagram socket和 TURN/UDP demux。Selected TCP pair 从 active candidate 的实际 source address 绑定 ephemeral port并连接 remote passive address；SDP port `9` 不用于 bind、listen 或 connect。STUN check 与后续 DTLS、SRTP/RTCP 和 SCTP packet 都复用同一条 connection，并使用 RFC 4571 的二字节 network-order length framing。Transmit 保存一个 bounded pending frame并延续 partial write；receive 跨 poll 保存 prefix/payload fragmentation、拆分 coalesced frames并忽略 zero-length null packet。EOF、oversize frame 或 I/O error只结束所属 transport，selected path error进入既有 terminal lifecycle；nomination 后不做无信令 hot failover。

TURN 支持 UDP long-term credential Allocate、Refresh、CreatePermission、Send/Data Indication 和 lifetime-zero deallocation；`turns:`、TURN/TCP、TURN/TLS、local passive 和 simultaneous-open 不在 contract 内。DTLS 只消费调用方注入的 PAL DTLS session，并验证 remote SDP fingerprint；H2Peer 私有的 `internal/libsrtp` 通过标准 Crypto PAL 保护 RTP/RTCP；调用方注入的 SCTP PAL 在 DTLS application record 上处理完整 SCTP packet。DCEP、WebRTC PPID 和 DataChannel stream mapping 仍由 H2Peer 拥有，stream table 使用 PAL allocation 动态增长，没有 two-stream 或固定 stream-count contract。

ICE candidate、STUN/TURN attribute、nominated pair、UDP bind/receive/send 和诊断格式化统一直接使用 `h2_pal_net_addr_t`；family 使用 `h2_pal_net_family_t`，PAL state 中的 port 保持 host order。STUN/TURN 和 DCEP 的 network byte order 由 H2Peer 逐字段读写固定宽度 byte，不把 wire buffer 强转为 C struct。Portable provider 不 include lwIP/POSIX socket header，不保存 `sockaddr`，也不根据 ESP、BK 或 host target 选择地址模型、兼容 include、byte-order macro 或 link dependency。

`libs/pal/providers/h2peer/internal/libsrtp` 使用 top-level `@h2_vendor_libsrtp` v2.8.0 verified archive，并把上游类型、PAL-backed Crypto/allocator bridge 与 wrapper contract 保持在 H2Peer package 内。它的 Bazel target 只对 H2Peer parent package 可见，不是独立 repository library 或 Public API。H2Peer 不链接具体 TLS engine、usrsctp、JSON、HTTP/MQTT signaling 或 target SDK，也没有 pthread/POSIX compatibility shim。Private libSRTP process-global init 在 live connection 间引用计数；并发 H2Peer owner 必须使用相同的 Memory/Crypto backend identity。SCTP provider 由 target composition owner 创建并注入：Desktop、ESP 和 BK 当前都使用 `libs/pal/providers/h2sctp`，并在销毁最后一个 H2Peer association 后才能销毁 provider。

`h2_peer_poll()` 使用调用方给出的 timeout 等待并派发 event queue，不再由 App task 同步驱动 socket。PAL callback 收到的 SDP、Opus 和 DataChannel payload 是 callback 期间有效的 borrowed buffer。`H2_PAL_ERR_WOULD_BLOCK` 是可重试的瞬态结果，不能消费调用方尚未成功提交的完整 message 或 frame。Portable connection 进入 `FAILED` 后，event 排空后的 poll 稳定返回 `H2_PAL_ERR_IO`；进入 `DISCONNECTED` 或 `CLOSED` 后稳定返回 `H2_PAL_ERR_CLOSED`，不能继续用成功 poll 掩盖 terminal transport。

连接完成后的 direct UDP receive 与协议处理都在 owner task：每轮先用 timeout 0 逐包处理，遇到 `WOULD_BLOCK` 或达到 16 包上限后检查 control、RTP 和 DataChannel readiness。用户侧 ready snapshot 处理完后，owner 用最多 `1 ms` 的 UDP receive 代替纯 sleep；若 transport 当前不能等待，才退回 `1 ms` sleep。SCTP 整体拥塞时只保留 DataChannel ready snapshot并优先处理 UDP ACK、timer 和 RTP；恢复可写后继续处理 snapshot，再取得下一批 ready bits。TCP、TURN 和 ICE selection 阶段同样保留单 owner 的 bounded receive 路径。DataChannel 最大 message 仍为 64 KiB；256 KiB 只是 association receive credit，由 target 注入的 Memory PAL 按实际接收量动态分配，不在 association 创建时预留整块内存。

Protocol owner 使用必填的微秒 monotonic clock 统计每轮耗时，并每五秒通过 `h2peer/perf` 汇总一次。日志中的 `command`、`send`、`transport` 和 `idle` 都按 `count/average_us/max_us` 输出；`idle` 包含等待合并 wakeup gate 的时间，其余类别只统计 active round。不能逐轮写串口日志，否则日志 I/O 会改变被测吞吐与调度。

Provider 不能把 mbedTLS、wolfSSL、libSRTP、H2SCTP-private、ESP-IDF、Armino 或 POSIX type 暴露给 H2Peer public header。HTTP、MQTT、WebSocket、JSON 和 cloud signaling 位于 WebRTC PAL 之上，也不能进入 H2Peer core。

## Wire Safety

GizOS-owned RTP、RTCP、STUN 和 DCEP codec 使用固定宽度整数逐字段读写 network byte order；SDP 使用 bounded line cursor。所有外部长度先与输入剩余空间和目标容量比较，再进行 copy。新 wire contract 不使用 C bit-field 或依赖编译器的 packed layout；portable provider 中继承的协议代码必须继续通过 malformed corpus、Pion interoperability 和目标编译器验证来收紧。

Opus RTP 固定使用 RTP v2 和 payload type 111，因此无 marker 的前两个 byte 是 `80 6f`。当前 PAL 不提供 silence 或 talkspurt 边界，连续发送的 Opus packet 保持 marker 为 `0`；不能把每次 `peer_send_opus` 都当作新 talkspurt。SRTP protect 完成后如果 UDP/TURN transmit 暂时 backpressure，portable connection 保存一个 bounded protected RTP packet，并在后续 poll 或下一次 send 前原样重发；不能用相同 RTP sequence 再次执行 SRTP protect。只有该 pending packet 仍无法 flush 时，新的 Opus frame 才返回 `H2_PAL_ERR_WOULD_BLOCK`，并且不消费新 frame、不推进它的 RTP sequence 或 timestamp。

接收方向在 SRTP decrypt 后按已协商的 RTP payload type 选择一个 audio 或 video decoder，不要求远端 SDP 提前声明固定 `a=ssrc`，也不把本地发送 SSRC 当作远端 SSRC。远端动态创建 Track、切换发送 SSRC 或省略 SDP SSRC attribute 时，合法的 Opus payload type 111 仍必须交付给 PAL callback；未知 payload type 保持丢弃。只有 Opus decoder 按同一 SSRC 的 RTP sequence 使用八个 packet 的 bounded reorder window：提前到达的 packet 暂存，缺失 sequence 在窗口滑出前到达时按序一起交付；第一包 future packet 同时启动 60 ms monotonic deadline，protocol owner 的后续 poll 即使没有新 RTP 也会在到期时为缺失 sequence 逐包投影 `opus == NULL && opus_len == 0` 的 loss marker，再按序排空所有已缓存尾包，让 codec owner 执行 PLC 而不压缩播放时间。迟到或重复 Opus packet 保持丢弃；超过 50 个 sequence 的跳变或 SSRC 变化作为新 sequence 起点，不批量生成静音。PCMA、PCMU 与 H264 不进入这个状态机，sequence gap 仍直接交付真实 payload 且不会收到 Opus loss marker。这是固定容量与固定 deadline 的 bounded reorder，不是自适应 jitter buffer。

## Desktop Pion Compatibility Gate

`libs/pal/providers/desktop/tests/webrtc/` 保存一个 backend-neutral PAL scenario。production gate 只链接 H2Peer：

```text
//libs/pal/providers/desktop:h2peer_pal_pion_test
```

共同场景通过 public `h2_pal_webrtc_*` wrapper 做基础互通验证：本地 STUN、UDP/TCP、TURN/UDP、DataChannel text/binary echo、payload-type-111 Opus echo、remote close 和一次 reconnect。它不再执行大 payload、多 DataChannel 压力、512 次 SID reuse、吞吐阈值或 wall-clock percentile。每个 test 自己启动、停止并 reap 本地 Pion fixture；证据来自 Pion `GetSelectedCandidatePair()`，不能用 requested mode 或 SDP 顺序代替。

性能 workload 属于 portable `projects/e2e/apps/webrtc-performance/app`，不属于 package test；DevKit 与 AMOLED 的 H2Loader launcher 位于 `projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/<board>`，AMOLED launcher 还通过 Wi-Fi PAL 的 `set_power_save` 选择 modem sleep 策略，用于对比省电模式对 RTP 抖动的影响。它在同一个 UDP PeerConnection 上保持 Packet 和 Event 两条长期 DataChannel，创建三条 request-scoped service DataChannel，先下载 10 MiB、再上传 10 MiB，并并发 Packet telemetry、Event echo 与 100 个 20 ms Opus RTP frame。`smoke` 执行一轮，`benchmark` 执行 10 轮并输出逐轮 JSON 和 median。满载与纯数据 throughput 的 median 比值必须不低于 80%。Desktop `benchmark` 要求每组 100 个 RTP frame 零丢包、零 duplicate、零 reorder、零 deadline miss、零发送侧 `WOULD_BLOCK`，并以 40 ms 业务预算加 2 ms host 调度容差检查到达间隔 p99。设备 `smoke` 允许最多 5 个网络丢包和 200 ms p99，发送侧 deadline miss 与 `WOULD_BLOCK` 只作为诊断指标，duplicate 和 reorder 仍必须为零。两种 profile 在发送最后一帧后都保留 1 秒 playout grace，再按实际收到的唯一 sequence 计算 missing。

## Build And Test

从仓库根目录运行 package build 和 host test：

```sh
bazel test //libs/pal/providers/h2peer:all
bazel test --config=macos_arm64 --nocache_test_results \
  //libs/pal/providers/desktop:h2peer_pal_pion_test
bazel test --config=macos_arm64 \
  //projects/e2e/apps/webrtc-performance/app:webrtc_performance_test
bazel run -c opt --config=macos_arm64 \
  //projects/e2e/targets/cc_binary/webrtc-performance:h2peer-performance -- \
  --profile=benchmark --runs=10 --transfer-bytes=10485760
```

Package test 使用 deterministic PAL fake 和 package-private provider fake，覆盖 C11/C++17 public header、PAL Log 路由与截断、完整 150-entry SID pool、双向 reset 顺序和 duplicate event、三条同时存在的 DataChannel、RTP backpressure、wire round-trip、malformed input、allocation failure、partial provider initialization、TURN fake-time refresh 和 exactly-once cleanup。Pion gate 只证明 Desktop PAL interoperability；target component build、firmware image、hardware 和 live GizClaw service validation 仍属于后续 backend migration。

ESP target 上有两项测量时必须显式控制的平台状态。第一，lwIP 把 `SO_RCVTIMEO=0` 当作永久阻塞，ESP Net PAL 的 `tcp_recv`/`udp_recvfrom` 因此把 PAL timeout `0` 实现为 `MSG_DONTWAIT` 轮询、把有界 timeout 实现为 `SO_RCVTIMEO` 阻塞读；H2Peer owner 用 timeout `0` 轮询 TCP/TURN socket 时依赖这一语义。第二，H2Loader App command service 的 BLE 广播会让 Wi-Fi 共存调度把 station 每秒睡眠约十次，即使 `WIFI_PS_NONE` 也是如此，AMOLED 上实测 UDP 吞吐因此降到三分之一；AMOLED launcher 的 `H2_WEBRTC_PERF_BLE_ADV=0` 在 workload 前暂停广播，产品在语音会话期间应采用同样策略。

ESP-IDF 和 BK 的 production firmware 必须通过各自 native build 入口验证。BK H106 还通过 image-owned memory contract 检查 H2Peer、其私有 libSRTP、H2SCTP 与 BLE required symbols，拒绝 Classic Bluetooth symbols，并保持 AP image 在物理 `2380 KiB` partition 内；不能通过扩大 partition 掩盖集成成本。
