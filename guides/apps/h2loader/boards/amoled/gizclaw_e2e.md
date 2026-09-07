# AMOLED GizClaw Session E2E

`gizclaw-e2e/amoled` 运行与 Desktop 相同的 portable GizClaw E2E App。普通 Voice 用例从创建连接开始使用 `libs/gizclaw` Session 注册、加载完整 `assistants` catalog、选择服务器提供的 Workflow、准备临时 Workspace 并管理 Conversation 输入及终态。测试只保存选择和清理账本，不维护另一份可用于业务决策的就绪状态。RPC 和 Group 等底层接口测试使用独立 fixture，不混用同一活动 Session 的状态修改接口。

## 构建

```sh
bazel build --config=esp32s3 \
  --define=H2_GIZCLAW_E2E_VOICE_ONLY=1 \
  --//tools/bazel:firmware_version=0.1.0-session-e2e \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
```

未指定 `VOICE_ONLY` 时仍运行完整 `all` suite，其中 Voice 使用同一 Session 路径。Launcher 继续使用已有的北京 E2E endpoint、公开 `deploy-default` fixture 和设备已保存的 Wi-Fi；不把 Wi-Fi 凭据编入固件。Voice 使用确定性的 16 kHz mono PCM，经真实网络上传，并在库的 PCM Track 上核验下行非静音音频；Testing Audio wrapper 同时 drain 真实麦克风并记录采集健康，但上传的仍是 fixture PCM；该用例不验收麦克风音质或扬声器听感。

## Session 验收

确认未注册时阻塞，注册后自动得到一致的 Profile/catalog，刷新及 catalog 副本一致，Workspace 经服务端确认后 `can_start=true`。对话创建后禁止再次启动准备；输入开始和结束分别验证 `conversation_input_open`，PTT 完成及 Realtime 挂断验证对应终态。释放 route 后重新允许准备；不同输入模式先释放空闲 route，再通过 Session 重新准备参数和绑定对话。重连销毁原 Session，并为新 Service 创建和注册新 Session。

保留既有 Voice 的完整文本/非静音回复、一次 PTT、一次取消、两轮 Realtime、历史播放、Track 替换及重连历史读取验证。Case 完成后释放回调和 Track，关闭并销毁 Session，再将 Service 交给原有精确资源清理账本；未确认删除不能计为回收。Session 的准备取消仍由本地并发测试验证，当前硬件 Voice 流程不声称覆盖在途准备取消。

## 设备运行

先用 H2Loader `scan` 和 `status` 核验实时 `board=amoled`、`target=esp32s3`、设备 UID、空 Stage 和 coredump baseline。使用 managed `send --file <package>` 与 `reboot upgrade --monitor` 安装，保存剔除协议帧的 UART 日志。不要擦除设备 Wi-Fi 配置。

要求安装后 status 的 APP/Partition 2 identity 与本次 package 一致、Stage 清空、`last_result=0`；日志包含 Session 逐操作业务断言、PTT 和 Realtime 结果、唯一 case terminal 及重复 final summary。只有 `selected=1 terminal=1 pass=1 cleanup_rc=0 retained_resources=0 complete=true exit_code=0` 且没有新 coredump 才能报告 Voice 硬件流程通过；它不是全量 215 项 API 验收。

## Resource suite

将上述构建参数换为 `--define=H2_GIZCLAW_E2E_RESOURCE_ONLY=1` 可单独验收新增 Resource state。该 suite 不运行音频，测试联系人、Profile、积分和分组状态；使用新 Peer，并在 Resource 关闭销毁后完成远端资源清理。最终日志必须为 `suite=resource selected=1 terminal=1 pass=1 cleanup_rc=0 retained_resources=0 complete=true exit_code=0`。本地替身测试不能代替这一 live 结果；空账户或空分组列表也不代表多页加载已验收。
