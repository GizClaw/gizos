# H2SCTP

H2SCTP 是 GizOS 自有的 portable SCTP provider。它实现
[`h2_pal_sctp_api_t`](/references/pal)，接收和发出完整 SCTP packet，供调用方在
DTLS record transport 上组装；它不打开 socket，不启动线程或 task，也不拥有 DTLS、
DCEP、WebRTC PPID 映射或 target selector。

## API Reference

[H2SCTP API Reference](/references/h2sctp)

PAL association contract、message/event 类型和 checked wrapper 以
`libs/pal/include/h2/pal/net/h2_pal_sctp.h` 为 source of truth；provider lifecycle 以
`libs/pal/providers/h2sctp/include/h2_sctp.h` 为 source of truth。

## Ownership

```text
caller DTLS transport
  -> h2_pal_sctp_association_input_packet()
  -> libs/pal/providers/h2sctp protocol state
  -> emit_packet callback
  -> caller DTLS transport
```

`libs/pal/providers/h2sctp` 只依赖 PAL。创建 provider 时借用 Memory PAL 和 Crypto PAL；每个
association 复制 scalar config 与 callback values。调用方必须先 close 全部 association，
再 destroy provider。存在 live association 时 destroy 返回
`H2_PAL_ERR_INVALID_STATE`，不会损坏 provider。

实现基于 RFC 9260，并覆盖 RFC 3758 partial reliability、RFC 6525 stream reset 和
RFC 8260 message interleaving。派生算法与 wire behavior 对照 pinned usrsctp revision
`01cc4e042e2235b29d9d489d89728a6f9ac063ed`；代码和测试全部位于 GizOS tree，
`LICENSE.usrsctp` 保留 BSD-3-Clause attribution。任何 H2SCTP production、test 或
validation target 都不得依赖、include、link、load 或执行 vendor usrsctp target。

## Budgets and flow

- `max_packet_size` 位于 `H2_PAL_SCTP_MIN_PACKET_SIZE..UINT16_MAX`；一个 callback
  只交付一个完整 packet。它只约束本端发出的 packet；入站 packet 以 IP datagram 上限
  65535 bytes 和调用方自己的 receive buffer 为界，对端使用更大的 path MTU 时不会被丢弃。
- `max_message_size` 同时约束发送和 reassembly；send budget 至少容纳一个最大 message
  加一个 packet，receive budget 至少 1500 bytes 且不小于最大 message。
- 双方协商 RFC 8260 后发送 I-DATA，并以 I-FORWARD-TSN 跳过 abandoned message；
  未协商 interleaving 时使用 DATA/FORWARD-TSN，并拒绝大于 16 KiB 的 message。
- stream state 按 ID sparse allocation；多个 stream 使用 equal-priority round-robin，
  不公开 provider-private scheduler。发送队列维护 association tail 和 per-stream unsent
  cursor；message append 与下一 stream fragment 选择不扫描全部历史 fragment。
- 连续、非 message-end 且未请求 immediate SACK 的 DATA/I-DATA 最多延迟到第二个 packet
  或 20 ms deadline 再确认；message-end、乱序、duplicate 和 immediate-SACK packet 立即确认。
  pending output、`WOULD_BLOCK` 或暂时 allocation failure 不丢失待确认状态，`service`
  通过同一 monotonic deadline 重试。

成功提交 message 表示 provider 已复制完整 payload。`H2_PAL_ERR_WOULD_BLOCK` 表示
没有消费输入，调用方必须以相同 packet/message 重试。`emit_packet` 返回
`WOULD_BLOCK` 时 provider 保留 exact packet、停止继续发送，并把 next deadline 设为
当前 `now_ms`；fatal callback error 只触发一次 `FAILED` transition。

stream reset callback 是 RFC 6525 request/response transition 的 exactly-once 投影，
不是每个 wire packet 的投影。重复或延迟的 outgoing-reset request 只重发既有 response，
不会再次投影 `INCOMING_RESET`；重复或延迟的 response 在对应 request 已完成后不会再次
投影 `OUTGOING_COMPLETED`。因此两个成功 callback 都同步返回后，provider 不再为该次
reset 产生迟到 callback；consumer 才可以复用同一个 numeric stream ID。`stream_test`
必须在 SID reopen 后重放旧 request 和 response，并证明 callback count 与新 stream
sequence state 都不改变。

## Time and failure semantics

所有 `now_ms` 都是同一 association 上非递减的 absolute monotonic milliseconds；
`UINT64_MAX` 保留为 no-deadline sentinel。`service` 只处理已到期工作，不 sleep 或 spin，
deadline addition 饱和到 `UINT64_MAX - 1`。同一 association 的调用由 caller serialization；
callback 递归 mutating operation 返回 `H2_PAL_ERR_BUSY`。

Malformed packet 使用 `TRUNCATED`、`FORMAT` 或 RFC silent discard 区分。可重试的
allocation/receive-credit failure 返回 `WOULD_BLOCK` 且不提交输入；close 无条件释放
timer、retained output、message、reassembly 与 sparse stream state，close 后不再 callback。

## Validation

```sh
bazel test //libs/pal:all //libs/pal/providers/h2sctp:all
bazel test --config=macos_arm64 --nocache_test_results  //libs/pal:all //libs/pal/providers/h2sctp:all
bazel query 'deps(//libs/pal/providers/h2sctp:all)'
```

Host tests 使用 deterministic Memory/Crypto fake 和可控制的 in-memory packet link，覆盖
handshake、CRC/wire vectors、三个以上 stream、fragmentation、loss/reorder/duplicate、
RTO/SACK（包括 delayed-SACK timeout、第二 packet、message-end、乱序和 output retry）、
partial reliability、receiver backpressure、reset、shutdown、abort、malformed input、callback
failure 与 cleanup。H2Peer/Pion cross-implementation gate、firmware build 和 device evidence
由消费该 provider 的后续迁移负责。

H2SCTP package test 不包含 wall-clock benchmark 或吞吐阈值。三个 stream 的 exact payload、双向传输、公平调度、backpressure 和 cleanup 继续由 deterministic test 覆盖；App-shaped DataChannel/RTP 性能由 `webrtc-performance` portable App 在 Desktop 与真实设备上测量。
