# AMOLED iperf Server

`iperf-server` 是触屏操作的设备间吞吐测试台，portable App 位于 `projects/e2e/apps/iperf-server/app`，AMOLED H2Loader target 为 `//projects/e2e/targets/h2loader_tar_zlib/iperf-server/amoled:package`。它复用 PAL-only `libs/iperf` 的 iperf3 server；网络与测速后台任务不调用 LVGL，屏幕通过同步 snapshot 显示状态、连接数量和各地址族的累计平均吞吐。

## 屏幕操作

启动时默认选择双栈，server 和 AP 均处于停止状态。点选 IPv4、IPv6 或 Dual stack，再点 Start server；其他设备连入显示的 Wi-Fi 后即可测速。运行过程中模式按钮锁定，点 Stop server 可取消尚未完成的控制/数据握手和正在运行的测试。后台先 join 并回收监听，再关闭 AP；停止后可以换模式、重新启动。

如果 join 或 AP 停止返回错误，屏幕显示 Stop failed 和错误码，并允许点 Retry stop 再尝试一次清理。清理成功前保留资源和停止状态，模式选择与 Start 保持锁定。销毁入口在 manager 退出后再尝试一次清理，持续失败则返回错误并保留 App 供后续重试；Runtime 和 callbacks 的生命周期覆盖成功销毁，底层 join/callback 仍遵守各自的阻塞合同。

网络启动失败也进入同一清理流程。AMOLED adapter 只尝试一次部分启动清理；失败时保留 AP 状态并返回错误，controller 继续持有清理责任，等待明确的 Stop/retry。没有已拥有资源时 Stop 成功返回，启动流程不会无限轮询 AP 停止。

| 模式 | AP 网络 | Server |
| --- | --- | --- |
| IPv4 | `192.168.4.1`，DHCPv4 | IPv4 TCP/UDP |
| IPv6 | `fd53:697a:6f73:626::1/64`，SLAAC；DHCPv4 停止且 IPv4 地址清空 | IPv6 TCP/UDP |
| Dual stack | 两者同时提供 | 两个独立地址族监听 |

固定 SSID 为 `GizOS-iPerf`，密码为 `gizosiperf`，WPA2，信道 6，最多四个 Wi-Fi station。iperf3 控制端口为 5201，TCP/UDP data 使用同一端口。每个地址族同时服务一个单 stream 测试，支持正向和 `-R` 反向；这个 App 不提供 SCTP、多 stream、bidirectional 或认证协议。

IPv6 是隔离的 on-link ULA 网络，Router Advertisement 的 router lifetime 为 0，不提供 Internet 默认路由或 DNS。RA timer 和 raw PCB 在 TCPIP task 中停止并回收后，Wi-Fi PAL 才销毁 AP netif。每次启动新建 netif，避免模式切换继承上一次地址或 DHCP 状态。临时 AP 不写入、清除或覆盖保存的 STA 凭据。

数据 block 上限为 128 KiB，控制 JSON 为 4 KiB，UDP 完整接收缓冲为 64 KiB；数据和 UI allocation 使用 PSRAM；368 × 448 RGB565 完整帧为 322 KiB，UI 使用 full-frame render。可选的 iperf 停止回调将阻塞 I/O 分为最多 100 ms 的等待，避免 idle client 阻塞 Stop。这个测速 image 只启动 H2Loader 串口管理，不初始化 BLE 控制器。启动时先初始化 Wi-Fi，LCD DMA chunk 上限固定为 8 行（5888 B），为 Wi-Fi/lwIP 保留内部 SRAM。

AMOLED board 的 DMA option 零值沿用 64 行默认值，只有这个 App 显式选择 8 行；驱动实际调用的同一 ceiling helper 覆盖默认值和边界测试。Display 的独立 source maintenance 只接受这一精确改动与 Touch 诊断增量，保留原 source/image/实板记录；当前修改后的固件需要单独实板验收，旧记录不自动认可 App 的 8 行配置。

## 构建和运行

```sh
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/iperf-server/amoled:package
```

按 H2Loader 的定向 `status -> send -> reboot upgrade -> status` 流程安装并检查 exact image identity。Display/Touch 打开且首次 render 完成后输出 `H2_IPERF_SERVER_UI_READY` 并确认 App；确认 UI 启动不等于完成无线吞吐或物理触摸验收。

设备间自动性能验收可增加 `--define=H2_IPERF_SERVER_AUTOSTART_MODE=4`、`6` 或 `46`，在真实 UI ready 后通过同一个控制器自动启动所选模式；默认 `0` 仍保持触屏选择、开机停止。这个入口不注入触摸事件，也不代替物理触摸验收。DevKit client target 为 `//projects/e2e/targets/h2loader_tar_zlib/iperf-client/devkit:package`，每次 server 换模式后 fresh boot，按本次 DHCP/SLAAC 地址族执行三轮 TCP/UDP 双向性能矩阵。

其他设备连接 AP 后，用 iperf3 测试；GizOS MCU 可使用 `h2_iperf_client_run()` 对屏幕地址发起同样的协议。ESP-IDF 自带 iperf2 示例不能作为该 iperf3 server 的客户端。

```sh
iperf3 -4 -c 192.168.4.1 -t 10
iperf3 -4 -c 192.168.4.1 -t 10 -R
iperf3 -6 -c fd53:697a:6f73:626::1 -t 10
iperf3 -6 -c fd53:697a:6f73:626::1 -u -b 10M -l 1200 -t 10
iperf3 -6 -c fd53:697a:6f73:626::1 -u -b 10M -l 1200 -t 10 -R
```

## 验证边界

`controller_test` 在真实 PAL loopback sockets 上覆盖三种模式、TCP/UDP 双向、官方 iperf3 client IPv4/IPv6 互通、握手/运行中取消、启动失败恢复和反复启停，并注入持续 AP stop、worker join、manager mutex 错误，验证销毁返回错误、保留所有权和恢复后重试成功。`ui_test` 把 Touch PAL down/up 事件送入生产 LVGL callback，完成十次选择/启动/停止与失败停止重试操作并验证 Display/Touch 关闭，输出实际 RGB565 渲染供检查。AMOLED 还需单独保留 exact package、UI/Touch 实板观察、外部 client 的无线测量和停止/重启记录；host 结果不能替代这些资格。
