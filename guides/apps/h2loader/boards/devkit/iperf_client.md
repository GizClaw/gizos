# DevKit iperf Client

`//projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package` 是与 [AMOLED iperf Server](/apps/h2loader/boards/amoled/iperf_server) 配对的设备间测速 image。portable App 位于 `projects/e2e/apps/iperf-client/app`，通过 Runtime 复用 PAL-only `libs/iperf` 与已有 E2E 结果记录器；入口持有临时 Wi-Fi association、地址就绪检查和 H2Loader 管理。

先在 AMOLED 选择 IPv4、IPv6 或双栈并开启 server，再启动 DevKit。Client 使用固定 SSID `GizOS-iPerf` / 密码 `gizosiperf` / 信道 6，关闭 Wi-Fi 省电，不启用 App 管理 BLE 服务，等待 15 秒完成 DHCP、RA 与 DAD；按实际获得的 IPv4/ULA 地址族选择矩阵。IPv6-only 不等待 IPv4 GOT_IP。测试不写保存的 STA 凭据；服务端换模式后需 fresh boot client，避免旧 netif 地址影响模式识别。

每个地址族执行三轮，每轮 TCP 正向/反向各一次（16 KiB block，不限速），UDP 5/10/20/40 Mbit/s 正向/反向各一次（1200 B datagram），每次 5 秒。单栈共 30 case，双栈共 60 case；双栈的两个地址族依次独立测量，不相加。UDP 配置速率是 offered load；性能报告取接收端实际吞吐，并保留丢包、抖动。

```sh
bazel test --config=macos_arm64 //projects/e2e/apps/iperf-client/app:client_test
bazel test --config=macos_arm64 //projects/e2e/apps/iperf-client/app:network_test
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package
```

定向核对设备 UID/port、P1、原 App、Stage、coredump 和恢复包后，通过 H2Loader send/upgrade 安装；禁止以 host loopback 或编译成功代替实板测速。`H2_IPERF_CLIENT_LINK` 记录实际地址、RSSI、BSSID、信道和无线 policy，`H2_IPERF_E2E_CASE` 的 target（如 `devkit-m46-f6-r2`）包含服务器模式、测试地址族和轮次。最终 `H2_IPERF_CLIENT_COMPLETE` 必须完整对应 30/60 个 case；其中 PASS 表示成功交换结果并接收 payload，不代表固定吞吐或丢包阈值。比较每个协议/方向/速率的三轮接收吞吐中位数，并保留全部原始结果和 source/package/image hash。

连接成功后的 setup 错误、测速失败或后续重连/凭据核验失败都会尽力断开临时 test AP，保留原始错误供 launcher 报告。只有完整 bench 返回成功时，DevKit launcher 才确认 image 并记录 `H2_IPERF_CLIENT_CONFIRMED rc=0`；失败 image 保持未确认。`network_test` 覆盖事件对齐、地址等待预算、失败清理，以及 disconnect 同时失败时的错误保留。

有效 bench 即使在首次连接或地址族发现前失败，也会执行最终保存凭据核验并尝试输出一次 `H2_IPERF_CLIENT_COMPLETE`。未运行矩阵记录 `matrix_started=0`、零个 case 和 `matrix_rc=INVALID_STATE`，尚未识别模式时 mode 为零；`saved_check_rc` 只记录结果，不输出密码或 signature。失败时资格 gates 全为零，后续清理或凭据核验错误不会覆盖第一项错误，失败流程保持未确认。

当前配对 App 只启用 TCP/UDP 正向和反向单 stream；`libs/iperf` 支持的 SCTP-over-UDP 尚未给两个 App 接线。ESP-IDF 原生 iperf2 示例不能与此 iperf3 server 互通。
