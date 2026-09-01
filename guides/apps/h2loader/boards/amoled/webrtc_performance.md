# AMOLED WebRTC Performance

`webrtc-performance` 在 AMOLED 上运行与 Desktop、DevKit 相同的 portable H2Peer workload：同一个 UDP PeerConnection 上的 Packet/Event 长期 DataChannel、三条 request-scoped service DataChannel、先下载 10 MiB 再上传 10 MiB，并并发 Opus RTP frame。workload、门限与 JSON 输出见 [DevKit WebRTC Performance](../devkit/webrtc_performance)；本页只说明 AMOLED launcher 的差异。

AMOLED launcher 不把 Wi-Fi credential 编进 image：先用 `wifi connect` 让设备保存 STA 配置，App 通过 Runtime `wifi_settings` 重连，没有保存配置时停在 `stage=wifi_settings status=ERROR`。取得 IP 后 launcher 用 `H2_WEBRTC_PERF_POWER_SAVE` 选择 Wi-Fi 省电策略（`0` 关闭 modem sleep，默认；`1` DTIM sleep；`2` listen-interval sleep），并在 `stage=power_save mode=<n> rc=<rc>` 中记录。image 使用与 [iperf](./iperf) 相同的 layout Wi-Fi/lwIP throughput profile。

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

构建 package：

```sh
bazel build --config=esp32s3 \
  --define=H2_WEBRTC_PERF_ENDPOINT=http://192.168.1.7:18080 \
  --define=H2_WEBRTC_PERF_STUN_URL=stun:192.168.1.7:3478 \
  --define=H2_WEBRTC_PERF_POWER_SAVE=0 \
  //projects/e2e/targets/h2loader_tar_zlib/webrtc-performance/amoled:package
```

按标准 `status -> send -> reboot upgrade -> status` 流程安装。串口 marker 使用 `H2_WEBRTC_PERF_AMOLED` 前缀，summary 行末尾附带 `power_save=<n>`；有效结果必须包含 exact package checksum、`amoled/esp32s3` identity、LAN endpoint、selected UDP ICE pair、双向 exact 10 MiB、完整 performance summary 和三类 heap 的 KiB checkpoint。
