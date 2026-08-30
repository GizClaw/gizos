# DevKit WebRTC Performance

`webrtc-performance` 是独立 H2Loader App，用同一 portable workload 测量 Desktop 与 DevKit 上的 H2Peer。它通过 Wi-Fi 和 operator 显式提供的 LAN Pion endpoint 完成连接、signaling 和 STUN。未显式设置 Wi-Fi environment 时，image 使用仓库允许共享的 `HAIVIVI-MFG` manufacturing/test network；其他 private Wi-Fi、个人 IP 或 private endpoint 不能提交到仓库。

DevKit 的 lwIP UDP receive mailbox 固定为 16 个 datagram，与 Tiga/H106 production target 保持一致。该 mailbox 只吸收 Wi-Fi/lwIP 短时调度抖动，不改变 H2Peer owner 每轮最多处理 16 个 UDP datagram 的公平性上限。

先启动 Pion fixture，其中 `192.168.1.7` 必须替换为 host 在测试 Wi-Fi 上的实际 IPv4：

```sh
bazel build //tools/webrtc-test-server:webrtc_test_server
bazel-bin/tools/webrtc-test-server/webrtc_test_server_/webrtc_test_server \
  --listen=0.0.0.0:18080 \
  --stun-listen=0.0.0.0:3478 \
  --turn-listen=127.0.0.1:0 \
  --candidate-ip=192.168.1.7 \
  --ice-mode=udp
```

先加载 SDK，再使用相同地址构建 package。两个 `--define` 只进入当前 firmware action 的 allowlisted CMake cache，不要求修改或提交 launcher source。Wi-Fi 通过单个 `H2LOADER_WIFI_CREDENTIALS` JSON environment提供，JSON 必须只包含非空的 `ssid` 与 `password`：

```sh
H2LOADER_WIFI_CREDENTIALS='{"ssid":"<ssid>","password":"<password>"}' \
  bazel build --config=esp32s3 \
  --action_env=H2LOADER_WIFI_CREDENTIALS \
  --define=H2_WEBRTC_PERF_ENDPOINT=http://192.168.1.7:18080 \
  --define=H2_WEBRTC_PERF_STUN_URL=stun:192.168.1.7:3478 \
  //projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/devkit:package
```

设备已有 H2Loader 且 command transport 可通信时，必须按标准 `status -> send -> reboot upgrade -> status` 流程安装，不能底层 erase 或 flash。有效结果必须包含 exact package checksum、`devkit/esp32s3` identity、LAN endpoint、selected UDP ICE pair、双向 exact 10 MiB、完整 performance summary 和三类 heap 的 KiB checkpoint。只有 firmware build 或 package Stage 不能算真实设备通过。
