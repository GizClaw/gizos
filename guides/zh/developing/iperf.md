# iperf

`libs/iperf` 提供只依赖 PAL 的 iperf3 兼容吞吐测试 client 和 server。它复用 iperf 3.x 的
线协议（control cookie、state byte、JSON 参数与结果交换、UDP packet header 和 connect
握手），因此 client 可以直接对官方 `iperf3 -s` 测速，server 也接受官方 `iperf3 -c`。数据流
支持三种传输：

- **TCP**：PAL raw TCP（`tcp_connect` / `tcp_send_timeout` / `tcp_recv`，server 端使用
  `tcp_listen` / `tcp_accept`）。
- **UDP**：PAL UDP，包含 iperf3 的 sequence/timestamp header、丢包、乱序和 RFC 1889 jitter 统计。
- **SCTP**：PAL SCTP association（[`h2sctp`](./h2sctp.md)）以 RFC 6951 UDP 封装承载。这是
  WebRTC data channel 依赖的同一条协议栈，可以用 iperf 语义测吞吐和可靠性。

## API Reference

[API Reference](/references/iperf)

`libs/iperf/include/h2_iperf.h` 是 Public API contract。`h2_iperf_config_t` 借用 Memory、
Net、Time PAL，可选 Crypto（随机 payload 和 cookie）、SCTP（SCTP 传输）和 Log。
`h2_iperf_client_run()` 阻塞完成一次测试并返回 `h2_iperf_result_t`：`local` 是本端测量，
`remote` 是对端在 EXCHANGE_RESULTS 中报告的数据。`h2_iperf_server_create()` 绑定 control
listener（以及 SCTP 封装 UDP socket），`h2_iperf_server_run_once()` 服务一个 client。

零值字段选择 iperf3 默认：port 5201、10 s、TCP 128 KiB / UDP 1460 B / SCTP 64 KiB block、
UDP 1 Mbit/s、SCTP 封装端口 9899（Linux kernel `net.sctp.udp_port` 的默认值）。

## 依赖和边界

- 库本身不包含任何 POSIX 调用；socket、时钟、随机数全部来自 PAL。
- 单 stream（`parallel == 1`），不支持 bidirectional、`--blockcount`、authentication 和
  `--get-server-output`；server 对这些请求回复 iperf3 `SERVER_ERROR`。
- SCTP 在 server 端是 passive association：第一个到达封装端口的 packet 决定 peer 地址和
  SCTP 源端口。与 Linux kernel iperf3 互通需要 server 主机开启
  `sysctl -w net.sctp.udp_port=9899`；macOS 和容器内核没有 SCTP，只能用本库自己的 server。

## 构建与测试

```sh
bazel test //libs/iperf:all
```

- `loopback_test`：PAL client ↔ PAL server，TCP/UDP/SCTP 正向、反向、按字节数结束和限速。
- `official_server_test`：PAL client ↔ 官方 `iperf3 -s`（`@h2_vendor_iperf//:iperf3`，
  从 esnet/iperf 3.21 源码用 Bazel 构建），TCP 与 UDP。每个 `iperf3 -s` 只绑定
  `127.0.0.1`，client 等到 iperf3 输出 `Server listening on` 才连接；探测端口释放后被
  占用导致 `unable to start listener` 时换新端口重试，最多 5 次，并有一个先占住端口的
  场景覆盖重试路径。
  断言按 iperf3 3.21 的计数方式放宽：
  - reverse 模式下，iperf3 server 在主线程处理 `TEST_END` 时不停止发送线程，而且
    `write()` 返回后才累加 `bytes_sent`。所以它报告的 sender 字节数最多比 PAL
    receiver 少一个 block。
  - iperf3 server 在 `TEST_END` 时停止计数，并丢弃 socket buffer 中还没读取的数据。
    按字节数结束的 TCP 场景因此不检查 receiver 下限。官方 `iperf3 -c` 要等下一个
    报告间隔 timer 才发现字节数已发完，这段延迟碰巧让 server 读完了数据。PAL client
    在最后一次 `write()` 之后立即发送 `TEST_END`，协议本身允许这样做。
- `official_client_test`：官方 `iperf3 -c` ↔ PAL server，TCP 与 UDP，含 64-bit UDP
  counter 和被拒绝的多流请求。
- `official_server_sctp_test`（manual）：PAL SCTP client ↔ 外部 Linux kernel SCTP
  `iperf3 -s`，通过 `H2_IPERF_SCTP_SERVER=<ipv4>[:<port>[:<udp-port>]]` 指定。

官方 iperf3 二进制只是本地测试工具，位于 `third_party/iperf.BUILD.bazel`，不参与任何
firmware 或 library 的构建。

## E2E App 与设备测量

`projects/e2e/apps/iperf/app` 把 client 封装成固定矩阵的 portable E2E App，
`//projects/e2e/targets/cc_binary/iperf:h2iperf` 提供 host 端 `server` 与 `client`，
`projects/e2e/targets/h2loader_tar_zlib/iperf/amoled` 是 AMOLED 的 H2Loader image。
server 在同一个封装 UDP socket 上顺序服务多条 SCTP association，只有 INIT chunk 才会
建立新的 passive association，前一条 association 的迟到 SACK/HEARTBEAT/SHUTDOWN 不会
把下一条带偏。见 [E2E 测试 App](/apps/e2e#iperf) 与
[AMOLED iperf](/apps/h2loader/boards/amoled/iperf)。
