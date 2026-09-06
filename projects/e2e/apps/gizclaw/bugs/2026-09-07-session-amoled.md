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
