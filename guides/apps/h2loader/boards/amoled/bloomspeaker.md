# Lua BloomSpeaker on AMOLED

该 image 在两块 368 × 448 AMOLED 上运行同一份 portable BloomSpeaker App 与 Lua 场景。AMOLED target 只负责 Runtime/BSP/provider 组装、component mapping、package identity 与 task policy；配对、conversation、Opus packet、两秒 liveness timeout、theme、mode crossfade 和清理都由 App owner 负责。

## Build and install

```sh
source ../firmware-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/lua-bloomspeaker/amoled:package
```

设备已有 H2Loader 时，按照 [AMOLED H2Loader](./h2loader) 的 managed stage/install/status/recovery 流程安装 package；不要用 erase 或 raw UART flash 绕过 Loader。记录同一次 build 的 package SHA-256，并确认两台设备安装完全相同的 protocol version。Reflash 后 BLE private address 可以改变；地址不是 device identity 或 firmware compatibility key。

## Controls and visible states

- 按住 BOOT/function 一秒进入黄色 pairing heartbeat；建立 session 前松开时，黄色旧 bloom 淡出，同时默认 bloom 淡入。
- 通话中按住 BOOT/function 一秒主动断开。所有 idle、pairing、talking、disconnecting/error visual mode 切换都在固定 particle pool 内 crossfade。
- 按住 PWR 两秒请求 software power-off。当前 bloom 先在 520 ms 内淡出并 present black frame，Lua job 返回且 native engine 完成清理后调用 power shutdown；App 侧 1000 ms deadline 保证 Lua failure 不会无限阻止关机。

Pairing beacon 是 25 bytes，携带 protocol version、ephemeral tag、ticket、epoch、claim state 与 target。候选按 `(ticket, tag)` 排序并相邻互选；三个同时 pairing 的设备最多形成一个不重叠的 two-party session，未匹配者继续等待。自动 passkey 来自公开 beacon 中的 ephemeral 值，只减少误连，不抵抗主动附近 MITM；生产系统必须增加预置 secret、QR/NFC out-of-band secret 或用户确认。

## Audio, timeout, and performance

Audio 使用 16 kHz mono Opus VOIP、20 ms cadence、DTX、FEC/PLC、固定 jitter reserve 与 4 KiB bounded transmit queue。Capture 遇到 backpressure 丢弃过期实时 frame，不阻塞 worker。每个有效接收字节（包括 silence/DTX）刷新 App-owned liveness；约两秒没有有效 audio bytes 后执行 `talking -> disconnecting -> idle`、关闭 iKCP/BLE、清空 remote level 并释放 Audio。

Lua pool 固定为 96 particles，talking 分为 64/32 slots，每侧最多 10 个 active heads。framebuffer trail 不创建 Lua trail object；idle、pairing、talking steady state 分别只更新计算得到的 bounded rectangle，crossfade 期间临时 full-screen fade。AMOLED target 单独选择 80 MHz pclk、DMA profile、12 dB mic PGA、aggressive AEC NLP、speaker volume 和 task priority/affinity；shared AMOLED board 的其它 image defaults 不变。

## Two-device acceptance

在两台串口日志中记录 firmware SHA、boot/reboot reason 与以下结果：

1. 同时按住 function，确认确定性 central/peripheral role、encrypted handshake 和双向 speech；上下 bloom 对本地/远端 level 均有响应。
2. 两端分别执行 explicit disconnect，确认 crossfade 后回到 idle，按键仍响应。
3. Pairing 中松开 function，确认黄色 outgoing 与默认 incoming bloom 同时淡变，没有 clear-frame jump。
4. 在 idle、pairing、talking 分别执行 power-off，确认当前画面可见淡出后才熄屏；重新上电验证 recovery。
5. 通过距离、屏蔽或关闭 peer 阻止有效 audio 至少两秒，确认接收端打印 RX timeout、两端最终 idle、之后仍可再次 pairing。
6. 连续观察 FPS/audio diagnostics 和 watchdog/reboot；silence 期间 DTX bytes 必须维持 session，不能被误判为 weak link。

真实 RF、speaker/microphone、visual timing、FPS 和 watchdog 结论只能来自这两台硬件；host test 或 firmware link 不能代替。缺少设备、serial access 或可控 RF attenuation 时，逐项记录 `SKIP`、缺少的 prerequisite 与 residual risk。
