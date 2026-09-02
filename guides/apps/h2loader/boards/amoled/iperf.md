# AMOLED iperf

`iperf` 是独立 H2Loader App，用 `libs/iperf` 的 PAL-only iperf3 client 在 AMOLED 上按固定矩阵测量 Wi-Fi/lwIP 的 TCP、UDP 与 SCTP-over-UDP 吞吐。它是 H2Peer 调优的分层基线：裸 UDP 给出 Wi-Fi 与 lwIP 的上限，SCTP case 使用与 H2Peer 相同的 `h2sctp` 协议栈和 1200 B packet budget，`webrtc-performance` 再叠加 DTLS/SRTP 与 H2Peer 本身的开销。

矩阵由 portable App `projects/e2e/apps/iperf/app` 拥有：`tcp_tx`/`tcp_rx` 参考值；UDP 上下行各 10/20/40 Mbit/s 与一组 1200 B datagram；SCTP 上下行各 1200 B 与 1400 B packet budget，8 KiB message。每个 case 默认 5 秒，case 之间间隔 500 ms；全部 case 顺序执行、不 fail-fast。

## Wi-Fi 与 lwIP 配置

image 使用 `boards/amoled/esp32s3/layouts/h2loader/sdkconfig.h2loader.defaults` 中的 Wi-Fi/lwIP throughput profile：静态 RX buffer 6、动态 RX 32、静态 TX buffer 8、PSRAM TX cache 16、RX Block Ack 窗口 12、tcpip 与 UDP mailbox 32、`LWIP_IRAM_OPTIMIZATION`。这是 ESP-IDF "minimum" 与 "default" rank 之间的有界取值：静态 RX 与 TX buffer 每个约 1.6 KiB 内部 DMA 内存并在 Wi-Fi init 时分配，动态 RX 与 TX cache 通过 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 落在 PSRAM；该选项同时要求 TX buffer 为 static 类型，因此不能改用 dynamic TX。board 自身的 `sdkconfig.defaults` 保持不变。

image 不携带 Wi-Fi credential。先在 Loader 或任一 App 中执行 `wifi connect`，设备保存 STA 配置后 App 通过 Runtime `wifi_settings` 重连；没有保存配置时 App 停在 `stage=wifi_settings status=ERROR` 并保持 H2Loader command-responsive。取得 IP 后 App 用 `H2_IPERF_POWER_SAVE` 选择的 Wi-Fi 省电策略：`0` 关闭 modem sleep（默认，用于吞吐与延迟基线），`1` 为 DTIM sleep，`2` 为 listen-interval sleep。 `H2_IPERF_BLE_ADV=0` 会在矩阵开始前暂停 H2Loader App command service 的 BLE 广播（默认 `1` 保持广播）：AMOLED 的 BLE/Wi-Fi 共存会让 Wi-Fi 在广播期间分时休眠，对比两种取值可以量化共存对吞吐的影响；暂停期间只能通过串口管理设备。

`H2_IPERF_ROLE_SERVER=1` 让镜像改为在 5201 端口运行 PAL iperf server（SCTP 封装端口 9899），host 用官方 `iperf3 -c <板子 IP>` 逐个方向驱动；这是分离设备收发路径与 host 工具的最直接方式。App entry task 在两种角色下都把自己提升到优先级 9，避免被 H2Loader 命令服务的串口轮询饿死。

layout 把动态 RX buffer 保持在 UDP mailbox 之上（64 对 32）：lwIP 零拷贝交付 Wi-Fi 帧，socket mailbox 里每个未读的报文都占着一个动态 RX buffer，两者相等时一个停止读取的 socket 就会让整个射频收不到任何帧，包括 TCP 控制连接。

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
  --define=H2_IPERF_BLE_ADV=1 \
  //projects/e2e/targets/h2loader_tar_zlib/iperf/amoled:package
```

设备已有 H2Loader 且 command transport 可通信时，按标准 `status -> send -> reboot upgrade -> status` 流程安装，不能底层 erase 或 flash。

## 预期表现

串口先输出 `H2_PAL_TASK_POLICY_READY`、`H2_IPERF_E2E_MEMORY checkpoint=entry ...`，随后 `H2_IPERF_E2E_AMOLED stage=wifi status=READY ip=... rssi=... channel=...` 、`stage=power_save mode=<n> rc=0` 与 `stage=ble_adv keep=<n> rc=0`。矩阵开始后每个 case 输出一行 JSON：

```text
I (12345) iperf_e2e: H2_IPERF_E2E_CASE {"v":1,"target":"amoled","id":"udp_tx_20m","proto":"udp","dir":"tx","block":0,"bitrate":20000000,"pkt":0,"rc":0,"rx_bps":...,"tx_bps":...,"packets":...,"lost":...,"jitter_us":...,"ms":...,"result":"pass"}
```

`rx_bps` 是接收端统计的吞吐，UDP 的 `lost` 与 `jitter_us` 来自接收端；portable App 的所有输出都经由 Log PAL，在 ESP 上以 `iperf_e2e` 作为 ESP-IDF log tag 输出。

2026-09-02 在信道 11、host 位于路由器另一子网、`H2_IPERF_POWER_SAVE=0` 下的实测（每 case 5 秒，射频波动大，单次结果不能当作门限）：

| 条件 | UDP 上行（板→host） | UDP 下行（host→板，20 Mbit/s 目标） | TCP 上/下 | SCTP 上/下（1200 B） |
| --- | --- | --- | --- | --- |
| BLE 广播开启（默认） | 2.3–2.4 Mbit/s，零丢包 | 2.0 Mbit/s，丢包 87% | 0.7 / 0.9 Mbit/s | 0.9 / 1.1 Mbit/s |
| `H2_IPERF_BLE_ADV=0` | 7.6–8.4 Mbit/s，零丢包 | 12.4 Mbit/s，丢包 33% | 5.3 / 4.8 Mbit/s | 5.8 / 5.1 Mbit/s |

广播开启时的差距来自 ESP-IDF 的 Wi-Fi/BLE 共存调度，而不是广播本身占用的空口：把广播间隔从 100 ms 放到 1000 ms、`esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` 都没有改善，只有暂停广播实例或停止控制器才恢复；`CONFIG_BT_CTRL_MODEM_SLEEP` 让上行回到约 5.4 Mbit/s 但仍远低于暂停广播，且需要单独验证稳定性。

TCP 窗口 11520 时受窗口限制；layout 现在把 `TCP_WND`/`TCP_SND_BUF` 提到 65535（缓冲在 PSRAM，internal free 不变），TCP 上/下升到 7.1 / 5.3 Mbit/s，剩余差距来自丢包引起的拥塞窗口收缩。UDP 下行的丢包发生在设备接收侧。同一块 devkit 上纯 ESP-IDF iperf 示例（无 BLE、无 H2Loader）为 TCP 22/35 Mbit/s、UDP 下行 20 Mbit/s；把 GizOS 的 iperf server 镜像用同一 layout 配置烧到 devkit（广播暂停）得到 TCP 16–20/21–22、UDP 19.5/20 Mbit/s，与 IDF 示例持平，说明 PAL 与 `libs/iperf` 没有额外开销，AMOLED 的差距来自板级射频。IDF 示例上 `SPIRAM_XIP_FROM_PSRAM` 约损失 30% TCP、`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`（Wi-Fi/lwIP 堆内存优先分配到 PSRAM，代码位置不变）约 25%，AMOLED 上 PSRAM 降到 80 MHz 无变化。矩阵结束后输出 `H2_IPERF_E2E_SUMMARY ... udp_tx_bps=... udp_rx_bps=... sctp_tx_bps=... sctp_rx_bps=...` 与 `H2_IPERF_E2E_AMOLED stage=summary status=PASS|FAIL ...`，并在 `matrix_start`/`matrix_end` 和每个 case 前后给出三类 heap 的 KiB checkpoint。有效结果必须包含 exact package checksum、`amoled/esp32s3` identity、LAN server 地址、完整矩阵 JSON 与 heap checkpoint；只有 firmware build 或 package Stage 不能算真实设备通过。
