# 设备 MHS 与 tool/v0 验收

独立的 `gizclaw_h2peer_device_live_test` 使用与其余 GizClaw E2E 相同的 fixture、注册和清理流程，连接测试 Peer，用设备身份创建 API key，通过 HTTPS 调用库内置的 MHS/tool-v0 provider。同一 case 也包含在完整 GizClaw E2E 中；独立 lane 不替代完整验收。Server 必须支持 SDK 0.22.0 协议，绑定的 RuntimeProfile manifest 必须声明可写的 `speaker.main/volume`（int，0–100）与 `speaker.main/muted`（bool）；注册本地 state 不会发布 manifest。

```sh
export H2_GIZCLAW_E2E_REGISTRATION_TOKEN='<E2E registration token>'
export H2_GIZCLAW_E2E_DEVICE_API_URL='https://ap.e2e.gizclaw.com'
export H2_GIZCLAW_E2E_AUDIO_URL='<public HTTPS Ogg/Opus URL>'
bazel test //projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_device_live_test \
  --test_arg=--endpoint=ap.e2e.gizclaw.com:9821 --test_output=errors
```

API URL 与音频 URL 显式传入；API key 只保存在测试内存，不输出 secret。设备通过 PAL HTTP 下载音频，Library 解码 Ogg/Opus，再写入按 PCM 时长消费的虚拟 PAL Audio sink。先运行本地 player/OTA 入口：playlist_set/repeat_set 后立即用 playlist_snapshot 回读条目数、标题与 revision，并确认拒绝越界写入和非法模式后状态不变。

远程音量通过 `PATCH /gizclaw/v1/device/mhs/v0/states` 一批写入 volume/muted，随后读取 Audio PAL 确认实际音量。播放列表、循环、播放、停止与 OTA 通过 `POST /gizclaw/v1/device/tool/v0/invoke`，body 包含 numeric registry 对应的工具名称与 typed args（例如 `{"tool":"audioplayer.play","args":{"index":0}}`）。成功结果位于 `result`：playlist.set 的 playlist_length 为 `result.playlist_length`，stop 的状态为 `result.state`。非法 playlist URL 必须在 Server 上被拒绝。

播放进度和 OTA 结果仍读取 `/device/status` 的 authoritative telemetry snapshot，不把工具接受回复当作执行完成。OTA 下载真实配置的 package 到计数 Stage sink，在 finish 故意拒绝校验，再检查 failed telemetry；该 sink 不写物理分区或重启。结束时撤销 API key、删除测试 Peer 并关闭 Service task。

本地 Service 测试覆盖 PAL 音量映射、增量 Ogg/Opus 解码、32 字节环形缓冲回绕、
HTTP 未结束时已输出 PCM、320→512 采样拼帧和末帧补零、失败列表原子性、在途下载
取消、response 完成前不重启、失败 response 取消动作，以及 OTA telemetry 的
借用字符串拷贝、混合 frame 拒绝与 WOULD_BLOCK 结果。实机音质、H2Loader 成功安装和
重启后的 succeeded 上报仍需设备验收。

AMOLED launcher 设置 `device_real_audio=true`，同一计数 sink 将 PCM 写入
`runtime.audio` 的真实 speaker track。该设备 case 的 OTA staging 仍是故意拒绝
finish 的测试后端，不会执行物理升级。

设备本地调用：

```c
h2_gizclaw_player_play(service, (h2_gizclaw_str_t){url, strlen(url)});
h2_gizclaw_player_get_status(service, &status);
h2_gizclaw_player_playlist_snapshot(service, &playlist);
h2_gizclaw_player_play_index(service, 1);
h2_gizclaw_player_play_index_at(service, 1, 20000);
h2_gizclaw_player_playlist_set(service, entries, 3);
h2_gizclaw_player_repeat_set(service, (h2_gizclaw_str_t){"all", 3});
h2_gizclaw_player_rate_set(service, 800);
h2_gizclaw_player_stop(service);
h2_gizclaw_ota_start(service, H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP,
                     (h2_gizclaw_str_t){0});
```

`playlist_snapshot` 和 `get_status` 只读设备已持有的状态，不发起网络请求；`play_index` 与 `play_index_at` 的索引越界在本地按 `H2_PAL_ERR_INVALID_ARG` 拒绝，不改动播放；`play_index_at` 的起点不小于条目已知 `duration_ms` 时同样拒绝，条目没有时长时从 0 播放。`playlist_set` 与 `repeat_set` 复用服务端 `playlist.set` / `mode.set` 的内部处理：校验失败时上一份 playlist 与播放都保持原样；`playlist_set` 是纯写入，不启动播放，专辑开始播放由随后的 `play_index(0)` 负责，末曲推进与循环由库按 repeat 模式负责，产品不要另行实现。`rate_set` 是播放器属性而非单次播放参数：接受 500–2000‰，越界返回 `H2_PAL_ERR_INVALID_ARG` 且速率不变，`get_status` 的 `rate_permille` 立即读回；E2E 在本地条目播放中设为 800 后恢复 1000。以上启动函数只复制参数、发布任务，返回 OK 表示接受。下载、解码、查询 firmware、
写入 staging 与进度上报由库拥有的 PAL task 执行。

仅运行 AMOLED 设备控制用例（完整 target 默认仍运行 all）：

```sh
bazel build --config=esp32s3 --define=H2_GIZCLAW_E2E_DEVICE_ONLY=ON \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
```

需先配置仓库要求的 `IDF_PATH` / `IDF_TOOLS_PATH`。AMOLED 使用本仓库生成并按提交固定版本的 32 秒 Ogg/Opus 测试音频 `playback_tone_32s_v1.ogg`，生成方法见 `projects/e2e/apps/gizclaw/data/README.md`：
`https://raw.githubusercontent.com/GizClaw/gizos/cf8dbdeba320984fc57ddba670dcf55237aa39cf/projects/e2e/apps/gizclaw/data/playback_tone_32s_v1.ogg`。
播放到 20 秒时检查服务端播放 telemetry，随后继续到整首自然结束，验证结束进度与播放时长；下载使用 64 KiB 环形缓冲。

真实下载、H2Loader 安装、重启及新镜像确认使用显式选择的 [AMOLED OTA hardware acceptance](/apps/h2loader/boards/amoled/gizclaw_ota_e2e) 入口；普通 device suite 的计数 Stage sink 不能作为真机升级通过的证据。

## 状态、工具与发现验收

SDK 通过 `client.tool.v0.list`（136）返回实际注册的 ClientTool 数字，并通过 `client.rpc.methods.list`（137）返回支持的 RPC family/version 数字。HTTP 控制端先用 `GET /device/tool/v0/tools` 发现工具，再调用 tool/v0/invoke。GizOS 根据 PAL 和产品 hooks 注册内置工具；DEVICE_FIND 与 SOCIAL_PING 由产品的静态 tool_handlers 表提供。旧的独立设备 RPC、字符串方法列表与 DeviceSettings hooks 已移除。

MHS state 表在初始化时借用到 Service deinit，产品用 read/check/write callback 提供亮度、locale 等状态。一次请求必须有 1–32 个唯一 key；写入先验证完整批次和全部 check，再执行 write。预检拒绝时硬件不变；应用中途出错不回滚，调用方重新 read。未知 key 返回 NOT_FOUND，写只读项或非法值返回 INVALID_ARGUMENT，失败的前置条件返回 FAILED_PRECONDITION。输出使用 callback 报告的实际值。详细合同见 [GizClaw 开发指南](/zh/developing/gizclaw#设备-provider)。

Factory reset 与 run.workspace.set 作为 tool/v0 过程保留 typed 产品 hook。Library 校验 keep_network/name/kickoff，回复完成后才交接到产品 owner；响应失败或停止会取消待执行动作。Workspace 切换由产品在 App owner 上调用 h2_gizclaw_session_select，Library 不越过 Session。

本地 `h2_gizclaw_protocol_test` 在 fake PAL 上向真实 SDK 输入 RPC frame，验证 SDK 工具列表、数字方法列表、已注册内置/产品工具调用、未安装工具与退休方法的 UNIMPLEMENTED，以及 MHS read/write 和 wire 错误映射。它保留 SDK 的完整 decode/dispatch/response encode 路径，不用本地数组查询代替发现。`h2_gizclaw_mhs_test` 验证表和 wire 校验、precheck 零写入、实际生效值、partial apply 与分配失败清理。Service 测试继续覆盖播放器、OTA 和回复后的产品交接。协议测试不等于真实网络、manifest 配置、设备音质或恢复出厂验收；live suite 未配置的产品 hooks 需随产品另行验收。

## Activity telemetry

`h2_gizclaw_telemetry_observation_t` 的 `ACTIVITY` observation 上报设备当前在用的
功能，服务端投影到 `PeerStatus.activity` / `activity_detail`；`h2_gizclaw_telemetry_system_t`
的 `firmware_version` 现在也会被服务端采用并投影到 `PeerStatus.firmware_version`。两者
的字段约束和拒绝行为见 [GizClaw 开发指南](/zh/developing/gizclaw)。E2E app 还没有上报
activity，服务端投影的验收随产品实现一起进行。
