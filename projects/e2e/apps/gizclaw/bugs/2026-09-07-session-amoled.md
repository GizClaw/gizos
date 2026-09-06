# AMOLED Session Voice E2E：PTT 回复超时

## 运行

2026-09-07，在 AMOLED / ESP32-S3 上通过 H2Loader managed send、reboot upgrade 安装 `0.1.0-session-e2e`。构建启用 `H2_GIZCLAW_E2E_VOICE_ONLY=1`，运行本 PR 的 Session Voice 路径；endpoint 为 `edge-bj-01.e2e.gizclaw.com:9821`，公开 fixture 绑定 RuntimeProfile `default`。本轮没有运行 all、device 或 OTA suite，也没有修改服务端配置。

Package SHA-256：`2135513d212c1fbb924541b869f7327a643a4d82ae63993d6d61d44dc71da835`，1,455,396 字节。App image SHA-256：`e7a0cf51314ac16bf9284a9a0f34007707d578595e41d7ce84238c6ede105c64`，2,139,264 字节。

## 结果

- Session create、register、refresh、catalog_copy、select、snapshot 的业务断言通过。注册得到有效 catalog，Workspace 已由服务器确认。
- Conversation create、Session audio_start、audio_end 和重复 end 调用返回成功。
- PTT 发送 35,942 字节，90 秒期限内没有完成回复；`rounds=0 playback_bytes=0 rc=-6`（TIMEOUT）。约 98 秒后 case 完成并报告 FAIL。没有证据足以归因于 Session、设备、网络或服务端。
- Session Conversation release、close、destroy、Workspace/Peer 清理与 Service deinit 完成；`cleanup_rc=0 retained_resources=0`。
- 安装后 APP 与 Partition 2 的版本/校验和匹配上述 package，`stage_valid=0 last_result=0`；Loader 分区校验和保持不变，coredump 仍为 `stored_bytes=0 blank=1`。
- 最终 summary：`selected=1 terminal=1 pass=0 fail=1 complete=true exit_code=1`，之后重复同一结果。
- 后续取消专项、Realtime 两轮、历史播放、Track 替换和重连历史检查因 PTT 失败未执行，不能记为通过。

日志保存在本地忽略目录 `build/validation/session-amoled/`：`uart.log`、`send.log`、安装前/Stage/安装后 status、coredump status 和 package metadata。Monitor 收集到重复终态后由操作者停止，其进程退出 130 不替代固件报告的 `exit_code=1`。

## 后续定位

使用同一公开 fixture、相同 Workflow 和 PCM，在 Desktop 与 AMOLED 分别记录请求/回复时间和终态，并关联服务端对应请求的处理状态。先确定回复缺失发生在哪一层，再调整实现或超时；不能通过扩大期限或删减文本/音频断言将本轮变为通过。

## 同固件复测

用户提示服务器可能刚重启后，以 `reboot app --monitor` 重跑同一已安装固件，未重新构建或升级。APP checksum 仍为上述 `e7a0cf…`，设备 UID `30eda0ae0f86`。

本轮仍为 PTT 超时：发送 35,942 字节，未收到下行音频，case 在 98,022 ms 报 `rc=-6`。最终 `selected=1 terminal=1 pass=0 fail=1 cleanup_rc=0 retained_resources=0 complete=true exit_code=1`。Session 与远端资源清理通过；复测后版本、分区校验和未变，coredump `stored_bytes=0 blank=1`。Realtime 等后续场景仍未到达；无法仅依据本轮判定服务器重启是原因。

复测日志及前后设备状态位于本地忽略目录 `build/validation/session-amoled-retry/`。这些日志来自旧 Session-only 固件，不包含随后新增的 Resource suite。

## 根因定位：缺少 AUDIO_INPUT_READY 握手

已核对实际 E2E Server `gizclaw --version` 为 `0.15.7`，服务启动时间为 2026-09-07 03:57:37 +08:00；本轮设备复测发生在 05:08–05:09，服务并非在该轮中重启。

设备日志与服务端 journal 的证据一致：

- 设备 uptime 12,076 ms 创建 Conversation，13,260 ms 记录 `input_committed`（57 帧、35,942 字节输入统计）；13,362 ms 才读取到 `peer_read type=9`，比提交晚 102 ms。
- pinned C SDK 定义 type 9 为 `AUDIO_INPUT_READY`，明确要求收到服务端对当前音频 BOS 的授权/安装确认后才能发送 Opus 包。
- `h2_gizclaw_conversation_wire_open_internal()` 在 BOS 发送返回成功后立即设置 `input_ready=true`；`conversation_request_poll()` 同样直接放开 `wire_ready`。接收端没有 `AUDIO_INPUT_READY` 分支，事件落到默认分支被忽略。
- Server 0.15.7 在 BOS 授权并交给 Agent 输入后广播 READY；Opus 是独立通道，未授权时到达的包直接丢弃，不缓存等候。
- 本轮临时 Workspace 为 `h2e2e-d6f2f4ed10fd6389-workspace`，Workflow 为 `doubao-realtime`，配置日志确认 `inputMode=push_to_talk`。05:08:18.740 服务端收到 `demo-1` 的 BOS；05:08:20.102 收到 EOS 并记录 `audioSent=0`。05:09:48 清理时仍为 `audioSent=0`、没有输出事件。

因此本轮是客户端音频输入握手缺失导致的空输入超时。注册、Workspace 准备和 Resource RPC 成功不能证明音频输入已获准；35,942 是客户端输入统计，也不等于服务端/模型已接收到同样的数据。

修复应在 Conversation 传输层区分 BOS 已发送与服务端 READY：只发送一次已成功的 BOS，持续处理事件，收到与当前 input stream 完全匹配的 READY 后才放行 Opus；READY 等待必须保留超时、取消和关闭语义，迟到/错误 stream 的 READY 不能打开下一轮。应补“READY 延迟时零上行、正确确认后完整输入、错误/重复/迟到确认和取消”的回归，再跑 AMOLED PTT/Realtime。此次定位未修改生产代码，也尚未完成修复后的 A/B 真机验证。

服务端已有同类协议回归 `TestAudioInputReadyPreservesAudioAcrossAuthorization`（tag `v0.15.7`），覆盖提前发送丢失输入与等待 READY 保留输入的区别。这里只核对其源码，不将其记为本机已执行测试。

原始服务端诊断日志保存在本地忽略目录 `build/validation/voice-timeout-diagnosis/`。源代码依据：本仓库 `libs/gizclaw/src/h2_gizclaw_conversation.c`；Server tag `v0.15.7` 的 `pkgs/gizclaw/peer_conn.go`、`pkgs/gizclaw/peer_input_ready_test.go`。
