# 标准设备 RPC 与 API 验收

独立的 `gizclaw_h2peer_device_live_test` 使用与其余 GizClaw E2E 相同的
fixture、注册和清理流程，连接一个测试设备，使用设备身份创建 API key，
通过 HTTPS HTTP API 反向调用库内置的设备 provider。同一 case 也包含在完整 GizClaw E2E 中；独立 lane 不替代完整验收。

```sh
export H2_GIZCLAW_E2E_REGISTRATION_TOKEN='<E2E registration token>'
export H2_GIZCLAW_E2E_DEVICE_API_URL='https://ap.e2e.gizclaw.com'
export H2_GIZCLAW_E2E_AUDIO_URL='<public HTTPS Ogg/Opus URL>'
bazel test //projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_device_live_test \
  --test_arg=--endpoint=ap.e2e.gizclaw.com:9821 --test_output=errors
```

API URL 与音频 URL 显式传入；API key 只保存在测试内存，不输出 secret。
设备通过 PAL HTTP 实际下载音频，库实际解码 Ogg/Opus，再写入按 PCM 时长消费的
虚拟 PAL Audio sink。测试先调用本地 player/OTA 入口，其中设备侧 `playlist_set` / `repeat_set` 写入后立即用 `playlist_snapshot` 回读条目数、标题与 revision，并检查越界写入和非法模式被拒绝后 playlist 与模式都保持原样；再检查远程音量、非法列表、播放列表、循环模式、播放进度和停止。
播放器进度和 OTA 结果均读取服务端 `/device/status`，不以 RPC 返回代替 telemetry 验收。
OTA 使用部署环境的 firmware metadata 和 HTTPS package URL，写入计数 Stage sink，
在 finish 时故意拒绝校验，检查服务端可读的 failed telemetry，绝不写物理分区或重启。
结束后撤销 API key，fixture 删除测试 Peer 并关闭所有 Service task。

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
h2_gizclaw_player_stop(service);
h2_gizclaw_ota_start(service, H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP,
                     (h2_gizclaw_str_t){0});
```

`playlist_snapshot` 和 `get_status` 只读设备已持有的状态，不发起网络请求；`play_index` 与 `play_index_at` 的索引越界在本地按 `H2_PAL_ERR_INVALID_ARG` 拒绝，不改动播放；`play_index_at` 的起点不小于条目已知 `duration_ms` 时同样拒绝，条目没有时长时从 0 播放。`playlist_set` 与 `repeat_set` 复用服务端 `playlist.set` / `mode.set` 的内部处理：校验失败时上一份 playlist 与播放都保持原样；`playlist_set` 是纯写入，不启动播放，专辑开始播放由随后的 `play_index(0)` 负责，末曲推进与循环由库按 repeat 模式负责，产品不要另行实现。以上启动函数只复制参数、发布任务，返回 OK 表示接受。下载、解码、查询 firmware、
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
