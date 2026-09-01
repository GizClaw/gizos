# AMOLED iperf

`iperf` 是独立 H2Loader App，用 `libs/iperf` 的 PAL-only iperf3 client 在 AMOLED 上按固定矩阵测量 Wi-Fi/lwIP 的 TCP、UDP 与 SCTP-over-UDP 吞吐。它是 H2Peer 调优的分层基线：裸 UDP 给出 Wi-Fi 与 lwIP 的上限，SCTP case 使用与 H2Peer 相同的 `h2sctp` 协议栈和 1200 B packet budget，`webrtc-performance` 再叠加 DTLS/SRTP 与 H2Peer 本身的开销。

矩阵由 portable App `projects/e2e/apps/iperf/app` 拥有：`tcp_tx`/`tcp_rx` 参考值；UDP 上下行各 10/20/40 Mbit/s 与一组 1200 B datagram；SCTP 上下行各 1200 B 与 1400 B packet budget，8 KiB message。每个 case 默认 5 秒，case 之间间隔 500 ms；全部 case 顺序执行、不 fail-fast。

## Wi-Fi 与 lwIP 配置

image 使用 `boards/amoled/esp32s3/layouts/h2loader/sdkconfig.h2loader.defaults` 中的 Wi-Fi/lwIP throughput profile：静态 RX buffer 6、动态 RX 32、静态 TX buffer 8、PSRAM TX cache 16、RX Block Ack 窗口 12、tcpip 与 UDP mailbox 32、`LWIP_IRAM_OPTIMIZATION`。这是 ESP-IDF "minimum" 与 "default" rank 之间的有界取值：静态 RX 与 TX buffer 每个约 1.6 KiB 内部 DMA 内存并在 Wi-Fi init 时分配，动态 RX 与 TX cache 通过 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 落在 PSRAM；该选项同时要求 TX buffer 为 static 类型，因此不能改用 dynamic TX。board 自身的 `sdkconfig.defaults` 保持不变。

image 不携带 Wi-Fi credential。先在 Loader 或任一 App 中执行 `wifi connect`，设备保存 STA 配置后 App 通过 Runtime `wifi_settings` 重连；没有保存配置时 App 停在 `stage=wifi_settings status=ERROR` 并保持 H2Loader command-responsive。取得 IP 后 App 用 `H2_IPERF_POWER_SAVE` 选择的 Wi-Fi 省电策略：`0` 关闭 modem sleep（默认，用于吞吐与延迟基线），`1` 为 DTIM sleep，`2` 为 listen-interval sleep。

## 运行

先在与设备同一 LAN 的 host 上启动 PAL iperf server，其中 SCTP 通过 RFC 6951 UDP 封装端口 9899 承载：

```sh
bazel run --config=macos_arm64 //projects/e2e/targets/cc_binary/iperf:h2iperf -- server --port 5201 --sctp-udp-port 9899
```

同一个 `h2iperf client <host>` 子命令可以在 host 上跑相同矩阵，用于校验 server 与网络路径。

构建 package 时通过 `--define` 提供 server 的 LAN IPv4，其中 `192.168.1.7` 必须替换为 host 的实际地址：

```sh
bazel build --config=esp32s3 \
  --define=H2_IPERF_SERVER=192.168.1.7 \
  --define=H2_IPERF_POWER_SAVE=0 \
  //projects/e2e/targets/h2loader_tar_zlib/iperf/amoled:package
```

设备已有 H2Loader 且 command transport 可通信时，按标准 `status -> send -> reboot upgrade -> status` 流程安装，不能底层 erase 或 flash。

## 预期表现

串口先输出 `H2_PAL_TASK_POLICY_READY`、`H2_IPERF_E2E_MEMORY checkpoint=entry ...`，随后 `H2_IPERF_E2E_AMOLED stage=wifi status=READY ip=... rssi=... channel=...` 与 `stage=power_save mode=<n> rc=0`。矩阵开始后每个 case 输出一行 JSON：

```text
I (12345) iperf_e2e: H2_IPERF_E2E_CASE {"v":1,"target":"amoled","id":"udp_tx_20m","proto":"udp","dir":"tx","block":0,"bitrate":20000000,"pkt":0,"rc":0,"rx_bps":...,"tx_bps":...,"packets":...,"lost":...,"jitter_us":...,"ms":...,"result":"pass"}
```

`rx_bps` 是接收端统计的吞吐，UDP 的 `lost` 与 `jitter_us` 来自接收端；portable App 的所有输出都经由 Log PAL，在 ESP 上以 `iperf_e2e` 作为 ESP-IDF log tag 输出。矩阵结束后输出 `H2_IPERF_E2E_SUMMARY ... udp_tx_bps=... udp_rx_bps=... sctp_tx_bps=... sctp_rx_bps=...` 与 `H2_IPERF_E2E_AMOLED stage=summary status=PASS|FAIL ...`，并在 `matrix_start`/`matrix_end` 和每个 case 前后给出三类 heap 的 KiB checkpoint。有效结果必须包含 exact package checksum、`amoled/esp32s3` identity、LAN server 地址、完整矩阵 JSON 与 heap checkpoint；只有 firmware build 或 package Stage 不能算真实设备通过。
