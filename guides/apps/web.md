# Web

Web 入口归 portable App 的 project owner。Web 不是 Mobile 子平台；它有独立的 lifecycle、toolchain 和 hosting contract。

## Opt-in fake Wi-Fi and modem

An embedding host can call `h2WebConfigureEnvironment()` on the packaged Web page before Start, then again to change the simulated network scenario. A session started without this opt-in rejects later environment injection; reinstall the page to enable fake radios. The version-1 object is copied to `Module.h2WebEnvironment`:

```js
{
  version: 1,
  wifi: { enabled: true, connected: true, ssid: 'Browser Wi-Fi', rssi: -48 },
  modem: { enabled: true, simPresent: true, registered: true, rssi: -70 }
}
```

Only opted-in platforms install the Web fake Wi-Fi STA/settings and modem PAL APIs; ordinary pages keep the canonical unsupported radios. Invalid objects reject platform creation. Radio snapshots are read on the browser main thread, and each platform owns mutex-protected volatile connection/settings/power state. Wi-Fi offers one open simulated AP, synthetic locally administered addresses and a documentation-range lease. Connect does not save; connect-and-save updates the simulation's saved credentials only after successful connection. A new platform starts with its configured simulated AP saved and connected. Settings are in-memory only. Modem supports open/close, SIM/registration/signal, data open/close and power policy; calls, GNSS and cell location remain unsupported. Identity explicitly names the simulation and exposes no fabricated IMEI/IMSI.

Real Fetch/WebRTC transports remain responsible for server communication. The HOST Netif is usable only when the browser is online and at least one simulated data path is open. Environment updates and radio operations trigger the existing Netif event flow. Closing both paths removes the usable default route, rejects new HTTP requests, WebRTC offers and outgoing data/Opus; applications observe the Netif change and manage their established sessions normally. The fake radios do not scan, connect or control the computer's real Wi-Fi/modem. This simulation is separate from evidence of a successful server connection.

`//libs/pal/providers/web/pal_core:fake_network_test` executes the real pthread WASM provider against deterministic browser network inputs, covering opt-in, invalid configuration, Wi-Fi saved-state semantics, modem data/power, no-SIM, offline HTTP rejection and teardown. The existing Netif test preserves the non-simulated browser contract.

## Runtime 输出到嵌入宿主

Web PAL 在主线程发布版本 1 的 `Module.h2WebOutputs`：`display.brightnessPercent` 是最后一次有效 Display PAL 背光设置，`speakerVolumePercent` 是扬声器主音量，`leds[id]` 包含板级 Periph 的 id/name、原始 RGBW `pixels` 和独立 `brightnessPercent`。这些都是实际 PAL 调用产生的输出，宿主只负责呈现，不据此改写固件状态。复制 canvas 到模型纹理不会复制 CSS brightness，因此嵌入宿主必须显式消费背光输出；100% 不应再被机身摄影棚曝光压暗。

App host 在产品 hardware hook 完成后，将 Periph API 绑定到公共 Web LED provider；产品已经提供自己的 LED API 时保持其实现。板级 mapper/Periph 拥有 LED_STRIP id、名字和 RGB/RGBW 像素数量，Web provider 支持 1..256 像素，提供 get_info、set_frame、set_solid、clear 和 brightness，未知 id 返回 NOT_FOUND、尺寸错误无副作用。借用的 Periph user/vtable/payload 必须覆盖 platform 生命周期。灯效、闪烁和亮度策略仍完全由 App/Runtime 决定，provider 不生成演示动画。`//libs/pal/providers/web/pal_core:outputs_test` 验证真实 WASM PAL 的颜色、亮度、无效输入与背光输出。

## Structure

```text
projects/example/
├── libs/web/tap-reset/            # Web presentation 与 App contract conversion
├── libs/web/demo_board/           # 示例 web board：240×240 屏幕、left/ok/back，device 与 plain 两套外观
├── targets/pkg_tar/lua-flappybird/ # 同一 Lua Flappy Bird App 的 Canvas archive（自带 shell）
├── targets/pkg_tar/mp4-player/    # WebCodecs MP4 播放 archive（自带 shell）
├── targets/pkg_tar/lua-script/    # h2_lua_web_app() smoke：Button args、OK 回调、默认 exit Button
├── targets/pkg_tar/lua-script-input/ # Button 输入合同（demo_board plain 外观）：多源合并、blur/隐藏/结束释放
├── targets/pkg_tar/lua-script-stop/ # 同一脚本：run_ms 触发与 Stop 相同的停止请求
├── targets/pkg_tar/web-board-{amoled,bk7258,k4b,szp,p4-lcd43}/ # 每块 board 的检查页；包名保持短，避免 Windows 路径超长
├── targets/pkg_tar/lua-script-extension/ # 加 extension：capability 与 exit_requested
└── targets/pkg_tar/<app>/         # tap-reset、display、log、qrcode、touch、lvgl-smoke、
                                   # starboy、lua-cosmic-drift、audio-system：
                                   # main.c + h2_web_app()

projects/e2e/targets/pkg_tar/
├── pal/                           # portable PAL E2E；?suite=browser 跑浏览器套件
├── libco/                         # libco Emscripten Fiber E2E
└── lua-runtime/                   # Lua Runtime 九 case Browser archive

projects/h2loader/
├── libs/web/                      # 可复用的 H2Loader JS/runtime/WASM build target
└── targets/npm_package/h2loader/  # @gizclaw/h2loader Browser SDK npm package

libs/pal/providers/web/pal_core/    # Canvas/Emscripten reusable PAL backend
boards/<board>/web/                 # 有屏幕的 board 的 web board 与外观：amoled、bk7258_v3_202405、
                                    # kickpi_k4b、szp、waveshare_esp32p4_wifi6_touch_lcd_4_3
tools/bazel/web/                   # Web 打包规则、HTML shell、board metadata 与布局工具
libs/lua/web/                       # 单个 Lua 脚本页面的 h2_lua_web_app() 宏与通用入口
```

`projects/<owner>/libs/web/<app>` 只能保存 Web-specific wrapper、required capability 和 portable App contract conversion；`targets/pkg_tar/<app>` 负责 lifecycle、Runtime assembly、HTML shell、package metadata 和最终可交付 archive。Canvas display、pointer handler、Memory、Time 和 Queue backend 属于 `libs/pal/providers/web/pal_core`，不能复制进 project entry。入口直接消费 Web PAL provider 并组装 Runtime，按自己的 App 合同启动与释放资源。`tools/bazel/web` 只生成构建产物和页面，不接管 C 生命周期。

## Build Boundary

Web wrapper C source 由 `//projects/example/libs/web/tap-reset:tap_reset_web` 提供 Bazel ownership。`@emsdk` 的 `wasm_cc_binary` 用真实 Emscripten toolchain 编译 App 与依赖，`@rules_pkg` 的 `pkg_tar` 再把 `index.html`、`index.js` 和 `index.wasm` 打成 serve-ready archive：

```sh
make test-web
```

`make test-web` 构建 `projects/example/targets/pkg_tar/` 与 `projects/e2e/targets/pkg_tar/` 下的 Web archive，验证归档根目录、入口引用和 WASM magic，通过 Emscripten/Node 执行 Web PAL、libco、portable PAL registry 与 fake Web Serial E2E，并在真实 Chromium 中运行各 archive 的 `:browser_test`（见下文 [Example 与 E2E 的 Web target](#example-与-e2e-的-web-target)）。每个 target 的 `:serve` 可在本机托管 archive。浏览器运行需要 HTTPS 或 localhost，以及 `Cross-Origin-Opener-Policy: same-origin` 和 `Cross-Origin-Embedder-Policy: require-corp`。Archive 内的 `_headers` 声明这两项响应头，仓库服务器会实际发送；其他静态服务器也必须配置，只有文件而未发送响应头不会启用 SharedArrayBuffer。直接调试使用：

```sh
bazel run //projects/example/targets/pkg_tar/tap-reset:serve
bazel run //projects/e2e/targets/pkg_tar/pal-core:serve
```

H2Loader Serial Web E2E 的 archive 是 `bazel-bin/projects/e2e/targets/pkg_tar/h2loader-serial/h2loader-serial.web.tar`。页面必须由用户点击按钮调用 Web Serial chooser；授权完成后，portable App 才能使用 opaque port ID。真实 status、只读 command 和 managed install 继续走同一 Host Core，install 由 launcher 提供精确 catalog SHA 与资源读取器。

[H2Loader Web SDK](./h2loader/apps/batch_loader/) 由 `//projects/h2loader/libs/web:h2loader_web` 提供 public JS module、matching Emscripten runtime 和 WASM，并通过 Web PAL/Host Core 执行 Web Serial、package inspection 和 managed lifecycle。`//projects/h2loader/targets/npm_package/h2loader:h2loader` 使用提交的 `package.json` 将完全相同的 SDK outputs 组装成 `@gizclaw/h2loader`；npm 是 Browser SDK 的分发格式，不提供 Node.js serial-port runtime。独立 workflow 在相关改动进入 `main` 后只发布尚不存在的版本。产品 frontend、hosting headers 和浏览器 UI 验收由 `GizClaw/www` 负责。浏览器保留 `SerialPort` 与 `File` 对象，C 只接收 runtime-scoped opaque handle 和 bounded slice。现有 H2Loader Serial Web E2E 继续验证底层协议与平台，不是产品 UI。

浏览器不能从本地 `file:` URL 加载生成的 `.wasm`。

Lua Flappy Bird archive 在 Canvas 上运行与 Desktop/AMOLED 相同的 portable App 和 ported Lua bytes。Web C `main()` 通过 Emscripten `PROXY_TO_PTHREAD` 运行在 Worker，PAL Task 使用 pthread Workers 和共享 Wasm 内存。`lua-runtime` 使用两个 Worker 并报告 `scheduler=multi-worker`；同一 Lua VM 内部的 coroutine 仍遵守 Lua 自身调度。HTML 不增加 Back/Stop 控件； 浏览器把 Escape 键通过 `h2_runtime_button_push_edge()` 写入映射后的 Runtime Button edge，由 portable App 作为唯一 Runtime Event consumer 识别并取消 job，HTML 不直接 修改 Lua/Host 状态。

仍使用 LVGL 的其他 Web App 与 Mobile 继续共享 `libs/lvgl:single_thread_config`。Web 的真实 Emscripten toolchain 需要 POSIX `strnlen` declaration，因此 `libs/lvgl:lvgl_web` 只增加 `_POSIX_C_SOURCE=200809L`，不复制配置或上游源码。

## Platform Boundary

当前 Web component 实现 Memory、Log、Time、Timer、Task、Queue、Sync、Display、Touch、Host Serial，Fetch HTTP、browser-seeded Crypto、Web Audio playback 与 microphone capture、WebCodecs H264/AAC decoding、基于 `navigator.onLine` 的 Netif 与 System Event、IndexedDB 持久 Filesystem（`h2_web_fs.h`），以及基于浏览器 `RTCPeerConnection`/`RTCDataChannel` 的 WebRTC 信令、DataChannel 和 caller-owned audio track。能力矩阵、错误语义与接入方式见下文 [Browser Runtime 能力](#browser-runtime-能力)。Media 由调用方拥有：页面把 `{stream: MediaStream, audio: HTMLMediaElement}` 注册进 `Module.h2WebRtcTracks`（以非零 wasm32 整数 token 为 key 的 Map），再用 `native_handle` 等于该 token 的 `h2_pal_webrtc_track_t` 调用 `h2_pal_webrtc_peer_set_track()`；`stream` 和 `audio` 可以省略其中之一，但不能都省略。Provider 不执行 `getUserMedia`、不构造 `Audio`、也不调用 `MediaStreamTrack.stop`；浏览器仍完成 Opus/RTP 编解码。调用方必须让 JS 对象、registry entry 和 C Track 存活到 `h2_pal_webrtc_peer_unset_track()` 成功或 peer 关闭；unset 等待 `replaceTrack(null)` 完成并清理远端播放。页面必须从用户手势启动首次连接，以满足 microphone permission。

Audio PAL 的麦克风通过 `getUserMedia` 与 `AudioWorklet` 采集，显式请求 `echoCancellation: true`，并在 Console 输出 track `getSettings()` 返回的实际状态；浏览器未确认启用时输出 warning，但仍允许采集。该参数是偏好，不能把请求成功视为实际消回声效果验收。采集由独立 16 kHz AudioContext 完成输入设备重采样，输出 mono S16LE，每帧 320 samples（20 ms）。八个可回收 buffer 同时约束 worklet message 和读取队列，耗尽时丢弃新输入，不阻塞 audio rendering thread。`mic_read` 至少需要 640 bytes，成功时填写实际 format；空队列的零 timeout 返回 `WOULD_BLOCK`，有限等待返回 `TIMEOUT`，同一 platform 的并发 read 返回 `BUSY`。调用方 Worker 在 pthread 等待中休眠，UI 线程继续处理浏览器事件。

麦克风首次 start 需要用户手势、HTTPS/localhost、浏览器授权和允许 blob worklet module 的 CSP；start 最多等待 30 秒。API 缺失返回 `UNSUPPORTED`，授权拒绝或设备不可用返回 `UNAVAILABLE`，运行中设备结束返回 `CLOSED`。Stop 幂等，清空队列、停止 owned native tracks、关闭独立 AudioContext，使未完成 start/read 返回 `CLOSED`；延迟到达的授权 stream 也会立即停止。销毁 platform 前必须 stop/cancel 并让活动 PAL call 返回。已启动的 stream 以 platform wasm32 address 为 key 暴露于 `Module.h2WebMicrophoneStreams`；页面可以将该 borrowed stream 放入 caller-owned WebRTC track registry，实现共用采集，但必须先 unset WebRTC track，再 stop microphone。

`//libs/pal/providers/web/pal_core:mic_test` 验证授权失败、延迟授权取消、device ended、超时、停止唤醒与重新启动；`:mic_browser_test` 在真实 Chromium 中用 48 kHz 合成麦克风设备验证 getUserMedia、AudioWorklet 与 16 kHz PAL PCM。自动测试不代表用户物理麦克风的收音质量验收。

`h2_pal_webrtc_peer_send_opus()` 仍返回 `H2_PAL_ERR_UNSUPPORTED`。`native_handle == NULL` 且带 read/write vtable 的 Opus Track（GizClaw 使用的 native provider 模型）通过 encoded transform 运行：provider 发送一条静音浏览器 track，把每个外发 encoded payload 替换为 `read()` 取得的 Opus packet，并把收到的 payload 交给 `write()`，不经浏览器解码播放；优先使用 `RTCRtpScriptTransform`（worker，CSP 需允许 `blob:` worker），缺失时使用 Chromium `createEncodedStreams`，两者都没有时返回 `UNSUPPORTED`。每个外发浏览器帧（20 ms packet time）替换为一个 Track packet，RTP timestamp 由浏览器生成，因此 Track 必须产生 20 ms Opus packet；read/write 只在 platform 拥有的 media task 中调用；收到的丢包不产生 zero-length marker。静音源依赖已解锁的 `Module.h2WebAudioContext`，页面须在用户手势中创建或恢复它；AudioContext 未运行时 set_track 在 Console 警告，上行在其恢复前没有帧。Task、Queue、Sync 和 Timer 由 pthread 实现；Task 使用 allocator 分配的真实原生栈，join 等待线程退出后回收栈和 Task 记录。线程取消是协作取消，在阻塞等待边界观察，不强制终止持有对象的 Worker。Web Serial、Fetch、WebRTC、WebCodecs、IndexedDB 与 Web Locks 的 JS 入口通过 `h2_web_main_thread.h` 同步代理到 UI，Promise 完成通过条件变量唤醒等待的 Worker；JS 对象和 DOM 留在主线程。Web PAL 不依赖 libco 或 Asyncify。`h2_web_platform_pump()` 保留为事件导入兼容入口，不负责 Task 调度，后台 Worker 自动处理网络事件。端口授权必须直接来自用户手势。不具备浏览器 API 对应能力的控制线读取、raw socket Net、BLE 和其余 PAL 使用 canonical unsupported provider 返回 `H2_PAL_ERR_UNSUPPORTED`，不能伪造成功或平台身份。

生产 Web App 必须定义 hosting headers、browser lifecycle、permissions、release packaging 和 supported-browser acceptance，不能把 smoke page 成功运行当作 Web 平台完成证据。持有 Runtime 的 Web entry 在 `pagehide`/freeze shutdown handler 返回前必须同步拒绝新操作、请求 task cancellation、使 pending Serial I/O 以 `CLOSED` 退出，由 Worker 等待活动 PAL 调用退出，再依次 join task、deinit Runtime 和销毁 platform；UI 生命周期回调不能阻塞等待 Worker。Web Serial 仅提供 Promise 形式的 reader/writer cancellation 和 port close；活动 session 的页面生命周期 shutdown 必须返回 `UNSUPPORTED`，只能同步失效回调并发起 best-effort 浏览器清理，不能声称这些 Promise 在 handler 返回前完成。需要确定性关闭证据的产品必须在页面仍可推进 event loop 时提供显式、可等待的 close 流程。

## Browser Runtime 能力

同一个 portable App 在硬件和浏览器上运行，差异只在 target 注入的 PAL。Web target 从 `libs/pal/providers/web/pal_core` 取得下列 provider，App 不感知浏览器，也不能为浏览器绕过 Runtime 检查。

| 能力 | Provider | 状态 | 自动化证据 |
|---|---|---|---|
| Netif / System Event | `h2_web_platform_netif_api()`、`h2_web_platform_system_event_api()` | 已实现 | `:netif_test`（Node）、`:browser_platform_test` 断网恢复 |
| 持久 Filesystem | `h2_web_fs_open()`（`h2_web_fs.h`） | 已实现 | `:browser_platform_test` 刷新持久、多标签页、清理、配额；`:async_test` 不可用存储 |
| HTTP | `h2_web_platform_http_api()` | 已实现 | `:browser_platform_test`（4xx、跨域预检、CORS 失败、流式、超时、取消、`NO_SPACE`）、`:async_test` |
| WebRTC | `h2_web_platform_webrtc_api()` | 已实现（浏览器媒体 Track 与 Opus Track） | `:webrtc_browser_test`、`:webrtc_opus_browser_test`（Pion 回环）、`:webrtc_track_test`、`:async_test` |
| 麦克风 / 扬声器 | `h2_web_platform_audio_api()` | 已实现 | `:mic_test`、`:mic_browser_test`、`:browser_platform_test` speaker |
| H.264 / AAC 解码 | `h2_web_platform_video_decoder_api()`、`h2_web_platform_audio_decoder_api()` | 已实现，依赖浏览器编解码器 | `:browser_platform_test` media（需 `H2_WEB_TEST_BROWSER` 指向 Google Chrome） |
| Display | `h2_web_platform_display_api()` | 已实现，背光同时发布宿主输出 | `:browser_platform_test` display |
| LED | `h2_web_platform_led_api()` | 基于板级 Periph 的模拟光学输出 | `:outputs_test` |
| Pref | `h2_web_platform_pref_api()`（localStorage） | 已实现 | `:pal_core_test` |
| Wi-Fi / 蜂窝 | 显式 opt-in 的 Web fake PAL | 模拟 radio 状态与数据链路；默认 unsupported | `:fake_network_test` |
| raw Net socket、MQTT、BLE、NFC、真实电池、板上 OTA | canonical unsupported | 不支持，返回 `UNSUPPORTED` | — |

### 默认网络（Netif）

浏览器只暴露一个默认路径：NAME `browser`、kind `H2_PAL_NETIF_KIND_HOST`。在线时状态为 `UP | LINK_UP | DEFAULT_ROUTE`；离线时 DEFAULT ref 返回 `NOT_FOUND`，`browser` ref 仍可查询但只有 `UP`。浏览器不暴露本机 IP、网关、DNS、MTU、MAC，provider 从不设置 `HAS_IPV4/HAS_IPV6`，也不填写这些字段。其它 kind、名称和 ID 返回 `NOT_FOUND`，不额外暴露 Wi-Fi 或蜂窝 Netif；对 `browser` 调用 `get_dns_servers` 或 `set_default` 返回 `UNSUPPORTED`，因为解析与路由归浏览器所有。没有布尔型 `navigator.onLine` 时所有调用返回 `UNSUPPORTED`。启用 fake radio 后还要求至少一条模拟数据链路可用，具体见前文模拟网络约定。

`online`/`offline` 事件在下一次 platform pump 中发布为 `H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED`，同一 pump 内相互抵消或重复的通知被去重。Consumer 必须用 `h2_pal_netif_status_is_usable()` 判断默认网络；`navigator.onLine == true` 只说明宿主有网络，目标服务是否可达（captive portal、防火墙、CORS、服务端故障）由实际 HTTP/WebRTC 结果决定。

公共契约调整：新增 `H2_PAL_NETIF_KIND_HOST`、`H2_RUNTIME_SYSTEM_NETIF_KIND_HOST` 与 `h2_pal_netif_status_is_usable()`。原先自行判断 `(flags & (UP | LINK_UP | HAS_IPV4)) == ... && kind != LOOPBACK` 的 consumer 改为调用该 helper；它对所有既有 native 接口的结果不变，对 HOST 接口不要求 `HAS_IPV4`。

### 持久 Filesystem

`h2_web_fs_open()` 在 App 启动前锁定并恢复一个持久根目录（Emscripten IDBFS，IndexedDB 数据库名为根路径），其余目录作为只读根（例如 `--preload-file` 资源）暴露；只读根下的任何修改返回 `UNSUPPORTED`，根以外路径返回 `NOT_FOUND`。

- 写入策略：`write()` 只改内存；`sync()`、写模式文件 `close()`、`rename()`、`remove()`、`mkdir()`、`clear()` 是提交屏障，返回前 IndexedDB 已提交此前全部修改。原子替换（写临时文件、sync、close、rename）返回 OK 后即可在刷新后读到。不依赖 `pagehide` 保存。
- 错误：配额 `NO_SPACE`，存储被浏览器阻止（部分隐私模式）`UNAVAILABLE`，缺少 IndexedDB 或 Web Locks `UNSUPPORTED`，其它 `IO`，提交超过 30 秒 `TIMEOUT`。失败的修改仍在内存中，下一次屏障重试；`h2_web_fs_get_status()` 给出最后一次失败的浏览器错误名与消息，打开失败同时写入 Console 与 `Module.h2WebFsLastError`。
- 刷新与多标签页：打开时持有 Web Lock `h2-web-fs:<root>` 直到 `h2_web_fs_close()` 或页面卸载。刷新会等待旧文档释放锁；另一个标签页在 `lock_timeout_ms`（默认 3 秒）后得到 `BUSY`，不会同时写同一份快照。Web Locks 需要安全上下文（HTTPS 或 localhost），否则 `UNSUPPORTED`。
- 清理：`h2_web_fs_clear()` 或对根目录 `h2_pal_fs_clear()` 删除全部内容并提交；用户也可在浏览器站点设置中清除。
- 限制：浏览器以 relaxed durability 提交 IndexedDB，已提交数据可跨刷新、关标签页和重启浏览器保留，但操作系统崩溃可能丢失最后一次提交；best-effort 站点存储可能在存储压力下被浏览器回收。Pref 仍使用 localStorage，跨标签页共享且无锁，后写者覆盖。

### HTTP

- 状态码与响应体原样返回，4xx/5xx 不会变成传输错误；`retry_count` 只在交付响应体之前对 408/429/5xx 与 IO 重试；所有尝试共享同一个 deadline，`TIMEOUT` 结束请求。`content_length` 取 `Content-Length`，响应带 `Content-Encoding` 时该头是压缩长度，此时报告实际交付的字节数。
- 响应体按浏览器 chunk 流式交给 `read_cb`；`response_buf` 放不下返回 `NO_SPACE`；无 allocator、无 buffer、无 `read_cb` 时计数并丢弃。
- `timeout_ms <= 0` 使用 10 秒默认值（与 CoreHTTP 相同）；超时与 `cancel_cb`（每 50 ms 轮询）都会通过 AbortController 中止进行中的 fetch，分别返回 `TIMEOUT` 与 `CLOSED`；task 取消返回 `CLOSED`。
- 离线时网络失败返回 `UNAVAILABLE`；其余 fetch 失败（DNS、TLS、mixed content、CSP、CORS）浏览器统一给出 `TypeError`，provider 返回 `IO` 并在 Console 记录方法、origin+path（不含 query）与原因，浏览器自身也会打印具体 CORS 原因。
- `interface_name` 返回 `UNSUPPORTED`。响应头名为小写，重复头以 `, ` 合并，只能读到 CORS safelisted 与 `Access-Control-Expose-Headers` 中的头，`Set-Cookie` 永不可见；`Host`、`Cookie`、`Origin`、`Content-Length` 等 forbidden header 由浏览器丢弃。重定向自动跟随，每一跳都要满足 CORS。

### WebRTC

信令为 offerer-only、non-trickle：`start_offer` 等待 ICE gathering 完成，最多 10 秒（`Module.h2WebRtcIceGatherTimeoutMs` 可调整）后提交已收集的候选并在 Console 警告。DataChannel、媒体 Track 所有权、背压（`WOULD_BLOCK` / `WRITABLE`）、断开与释放语义见 `h2_web_platform.h`。`FAILED`/`CLOSED` 后调用方关闭 peer 并创建新 peer 重连；浏览器离线约 5 秒后 peer 进入 disconnected，随后 failed。Chrome 用 mDNS `.local` 名称代替 host candidate，服务端需支持 mDNS 或提供可达的 STUN/TURN。远端音频 `play()` 可能因缺少用户手势被拒绝。

### 音频、视频与 Display

- 麦克风：16 kHz mono S16LE、320 samples/帧；需要用户手势、安全上下文与授权。拒绝或无设备 `UNAVAILABLE`，缺 API `UNSUPPORTED`，设备移除 `CLOSED`。
- 扬声器：track `write` 按播放时钟限流，最多排队 `track_queue_frames`（8）帧，超时返回 `WOULD_BLOCK`；track 开始或排队音频播完后，下一帧从 AudioContext 时钟之后 80 ms 开始，吸收主线程卡顿（代价是 80 ms 输出延迟），排队音频曾经播完时 Console 警告一次；`drain` 等到已排队音频（含 AudioContext 输出延迟）播放完毕；`close`/`stop_speaker` 立即停止（打断）。音量通过常驻 GainNode 立即作用于已排队音频。AudioContext 受 autoplay policy 限制处于 suspended 时播放时钟停止：Console 立即警告一次，有限 timeout 的写入到期返回 `WOULD_BLOCK`，无限等待会一直等到 AudioContext 恢复，页面必须在用户手势中创建或恢复 AudioContext。
- 解码：WebCodecs H.264 Annex-B 输出单 plane RGB565（stride = width×2），AAC-LC 输出 S16LE；frame 由 allocator 持有，release 后释放；`reset` 后可重复播放；WebCodecs 在 B 帧重排或 flush 时多出的输出不再判为错误。开源 Chromium（含测试用 pinned Chromium）不带 H.264/AAC，configure 返回 `UNSUPPORTED`；Google Chrome、Edge、Safari 提供这些编解码器。
- Display：必须先 `open`，未打开或已关闭时 `get_info`/`draw_bitmap` 返回 `INVALID_STATE`；Canvas 尺寸取自 `h2_web_platform_config_t`，页面中 `<canvas>` 的 width/height 必须一致（H106 为 240×240）。

### 生命周期

`h2_web_platform_destroy()` 返回 `BUSY` 并保留 platform，直到 task 全部 join、挂起在浏览器 Promise 上的 PAL 调用返回、扬声器 track 关闭；调用方继续 pump 后重试。正常关机顺序：请求 App 停止并 pump 到 task join，`h2_runtime_deinit()`，`h2_web_fs_close()`，`h2_web_platform_destroy()`。

### 服务端要求

浏览器中所有跨域 HTTP 都受 CORS 约束：

- 所有响应（包括 4xx/5xx 与重定向目标）返回 `Access-Control-Allow-Origin`（请求不带 credentials，可用 `*`）。缺少时浏览器返回 `IO`，状态码与响应体不可见。
- 带自定义头或 `application/octet-stream` 的请求会先发 `OPTIONS` 预检：返回 2xx，`Access-Control-Allow-Methods` 包含实际方法，`Access-Control-Allow-Headers` 包含全部自定义请求头（GizClaw 信令为 `content-type, x-giznet-public-key, x-giznet-timestamp, x-giznet-nonce`），建议 `Access-Control-Max-Age`。
- App 需要读取的非 safelisted 响应头列入 `Access-Control-Expose-Headers`。
- HTTPS 页面不能请求 `http://`（mixed content）；所有 App 使用的 endpoint 必须提供 HTTPS。
- WebRTC 服务端需接受 non-trickle offer（answer 携带全部候选），并支持 mDNS 候选或可达的 STUN/TURN。

### Consumer 接入示例

```c
#include "h2_web_fs.h"
#include "h2_web_platform.h"

h2_web_platform_t *platform = h2_web_platform_create(
    &(h2_web_platform_config_t){.display_width = 240, .display_height = 240});

// Before the App starts: lock and restore persistent data.
static const char *const readonly[] = {"/data/assets"};
const h2_web_fs_config_t fs_config = {
    .persistent_root = "/data/gizclaw",
    .readonly_roots = readonly,
    .readonly_root_count = 1u,
};
h2_web_fs_t *fs = NULL;
h2_pal_result_t rc = h2_web_fs_open(platform, &fs_config, &fs);
// rc: BUSY (other tab), UNSUPPORTED/UNAVAILABLE (storage), NO_SPACE, IO.

h2_runtime_config_t config = {0};
config.fs = h2_web_fs_api(fs);
config.netif = h2_web_platform_netif_api(platform);
config.system_event = h2_web_platform_system_event_api(platform);
config.http = h2_web_platform_http_api(platform);
config.webrtc = h2_web_platform_webrtc_api(platform);
// ... remaining Web providers and canonical unsupported APIs.
```

### Example 与 E2E 的 Web target

`targets/pkg_tar/<app>/main.c` 直接创建 Web platform、组装 Runtime、调用 App，并按依赖顺序清理。`-sPROXY_TO_PTHREAD=1` 已让 main 在 Worker 上运行，Web PAL 自己的 Worker 负责导入事件；入口无需再套 Host task。需要输入的入口显式调用 `h2_runtime_input_start()`，结束时先检查 `h2_runtime_input_stop()`，成功后 deinit Runtime 并释放 provider。Filesystem、LVGL、音频任务只由使用它们的入口初始化和清理；无法确认消费者已停止时保留其依赖并打印 `result=FAIL ... retained=1`。LVGL smoke 没有停止回调，入口单独启动一个 PAL task，在 Stop 或测试期限到达后请求协作取消并 join；其余入口直接运行 App。`h2_web_app()` 位于 `//tools/bazel/web:web_app.bzl`，生成 `.web.tar`、`:serve` 与 `:browser_test`，只负责编译、页面资源、预加载和打包。

板级配置直接写在 entry：在需要的时点准备资源，把 `power`、`periph`、`input`、`pwm_switch` 或 component mapper 放进 `h2_runtime_config_t`，并在最后一个消费者结束后释放。`web-hardware/main.c` 演示 component 42 到 peripheral 501 的显式映射，以及配置失败时只释放一次、释放失败时保留文件和依赖的行为。没有统一 hardware hook 表或通用 run 对象。`h2_web_app()` 的 `copts` 只追加到调用方源文件；生成的 board header 仅含常量 metadata。

### Web board

浏览器页面跑在一块 web board 上，它是实体 board 在浏览器里的对应物。`//tools/bazel/web:web_board.bzl` 的 `h2_web_board()` 声明这块板提供的外设和外观：

- `display_width` / `display_height`：屏幕（canvas）像素尺寸；
- `buttons`：有序的 Button 名到键盘键（DOM `KeyboardEvent.key`，可为空）的映射，最多 8 个，第 i 个是 periph id i + 1；
- `skins`：外观名到 HTML 文件（可带 `<style>`）的映射，一块板可以有多套外观，`default_skin` 指定默认那套。

页面 shell 把组件创建进外观的插槽：`data-h2-slot="display"` 放 Canvas（`#canvas`），`data-h2-slot="controls"` 放 Start/Stop，`data-h2-slot="status"` 放状态行；缺少的插槽使用普通容器。入口按需要调用 `h2WebBindButton(index, key, name)` 绑定键盘键和 `data-h2-button="<name>"` 元素。JavaScript 合并键盘和指针来源，只有按下状态变化才进入 128 项 FIFO；失焦或隐藏释放所有来源。入口 Worker 用 `h2WebTakeButtonEdge()` 取边沿，再调用 `h2_runtime_button_push_edge()`；0 表示空、-1 表示溢出，正值编码为 `(index + 1) * 2 + pressed`。`h2WebUnbindButtons()` 释放监听器并清空 FIFO。Stop 只设置 `Module.h2WebStopRequested`，由入口决定停止策略。页面根据 `H2_WEB_APP` 控制台标记更新状态，不拥有 Runtime/provider。

组装可运行 target 时选 board 和其中一套外观：`h2_web_app(board = "...", skin = "...")`，`skin` 省略时用该板的 `default_skin`，板上没有 `default_skin`（包括没有外观）时用朴素的 `default_layout.html`；显式给出的 `skin` 必须是板上的外观，否则 analysis 阶段失败并列出该板的外观。`h2_web_app()` 生成 `h2_web_board_config.h`，包含 `H2_WEB_BOARD_WIDTH/HEIGHT`、Button 数量以及名字/键表；入口自行选择是否使用这些常量以及如何映射到 Runtime。不指定 board 时用 `//tools/bazel/web:default_board`（240×240、无 Button、朴素布局 `default_layout.html`），所以现有 example 页面不变。board 与外观放在该 board 自己的目录旁 `boards/<board>/web/`（下游产品 board 同理），同一块板上的所有 App 复用。GizOS 为有屏幕的五块 board 提供了 web board：`amoled`（368×448，`boot`、`power`）、`bk7258_v3_202405`（800×480，`prev`、`next`、`menu`，对应实体板的三个 ADC 键）、`kickpi_k4b`（1024×600，`action_button`）、`szp`（320×240，`boot`）、`waveshare_esp32p4_wifi6_touch_lcd_4_3`（480×800，`boot`），尺寸与 Button 名取自各自 BSP。`amoled` 的 `device` 外观按 Waveshare 官方尺寸图用 CSS 画出实物（黑色表壳 37.60 × 45.20 mm、屏幕 28.70 × 34.94 mm、右侧从上到下 BOOT、USB-C、PWR）；其余四块的 `device` 外观目前是按 BSP 参数画的示意图（屏幕尺寸与方向准确），之后按实物资料替换。外观只用 CSS 绘制，不嵌入厂商图片。无屏幕的 `devkit`、`esp32p4_func_ev_board_v1_4`、`waveshare_esp32s3_a7670e_4g` 暂不提供。

只运行一个 Lua 脚本的 App 不需要 `main.c`：`//libs/lua/web:lua_web_app.bzl` 的
`h2_lua_web_app(name, script, board, skin, exit_button, extension)` 用
`h2_lua_resource()` 嵌入脚本，并把通用入口 `src/h2_web_lua_app.c` 交给
`h2_web_app()`。板上每个 Button 按板上顺序得到 Runtime component id 1..N，脚本从
`args.<name>` 读取；Button event 转发给 Lua job。`exit_button` 必须是板上的 Button（否则 analysis 失败），它的事件不进入脚本，默认在 release
Action 上取消 job，页面 Stop 同样取消 job 并以 OK 结束。job 第一次进入
WAITING 时输出 `stage=ready`；脚本失败时打印 `H2_WEB_LUA_APP job state=...`
并以 FAIL 结束。`extension` 是定义 `h2_web_lua_app_extension`
（`//libs/lua/web:lua_app_extension` 只提供声明）的唯一 cc_library；没有 extension 时入口
不引用该符号。`register_host` 在 Host start 前调用一次，返回非 OK 时不 start、
不提交 job，App 以该结果 FAIL；`exit_requested` 接收 exit Button 的每个
Runtime event（Down、Up、Action），返回 true 取消 job 一次，并取代默认的
release 规则，例如只接受长按。`run_ms` 非零时在该时长后发出与页面 Stop
相同的停止请求（用于测试）。`//tools/bazel/web:web_board_argument_test` 覆盖 board 参数校验（屏幕尺寸、超过 8 个 Button、非法 Button/外观名、未知 `default_skin`），`//libs/lua/web:lua_web_app_argument_test` 覆盖 `run_ms` 范围。其余参数原样交给
`h2_web_app()`。每个 package 只能有一个 `h2_lua_web_app()`/`h2_web_app()`。
这些宏内部 label 都用 `Label()` 解析到 GizOS，下游仓库可以直接 load
`@gizos//tools/bazel/web:web_app.bzl` 与
`@gizos//libs/lua/web:lua_web_app.bzl`。

Lua Display 在获取时启动公共 `$lua/display` worker，`present/end_frame` 等待该 worker 完成当前帧。Lua 通用入口自己拥有 platform、Runtime 和 Lua job，Stop 只取消 job，随后执行 checked shutdown，等待消费者清理。慢传输不会触发额外的入口任务取消；后端一直不返回时，清理可能无限等待。Lua 清理或 Runtime input join 失败时，入口保留仍需的依赖，报告 `result=FAIL ... retained=1`，并拒绝重复启动；正常结束前解除页面输入。provider destroy 失败时保留 provider。最终释放边界是页面／Wasm module 销毁；普通脚本错误在清理成功后完整释放。

入口的 configuration 借用对象必须覆盖消费者的完整生命周期；需要在失败返回后保留的对象使用 static 或 owned heap 存储。通用 Lua 入口使用静态 board metadata、Lua resource 和拥有的 provider/Runtime 指针。`//libs/lua/web/tests/lifecycle:all` 编译并调用实际入口，在真实浏览器中通过 C assertions 验证正常释放、普通脚本错误、慢传输 Stop、传输故障、Display/input join 失败和外部借用 FS 忙时的保留。已删除的入口任务没有 join 故障场景；原测试标签作为 input join 测试的 alias 保留。

#### Lua 脚本配置与预算

`h2_lua_web_app()` 的 `vm_memory_limit_bytes` 默认 524288（512 KiB），允许 65536..16777216；`source_limit_bytes` 默认 131072（128 KiB），允许 1..1048576。两者必须是整数，不能传入 bool 或字符串。默认调用的预算不变，较大的程序由自己的 artifact entry 显式选择预算；Display 的 VM 计费发送缓冲兼任 baseline，需要宽×高×2 字节加元数据，Host allocator 另持有一份绘制缓冲；现有应用还需脚本与图层缓存空间，因此 AMOLED、800×480/480×800、1024×600 board 示例分别显式选择 1、2、3 MiB VM 预算；可接受的配置不保证任意程序都能在该预算内运行。超限源码仍按 Host resource 校验失败，VM 分配耗尽仍报告 job failure，不自动扩容。

可选 `script_args` 是最多 16 项的 string-to-string 字典。名字为 1..32 个 ASCII `[a-z0-9_]` 字符，值为最多 256 个可打印 ASCII 字符，允许空值、引号和反斜线；控制字符和非 ASCII 内容会被拒绝。宏按名字排序并转义为固定 C 字符串，参数在提交时复制为 Lua `args` 字符串，不执行 Lua 表达式。任何名字与所选 board 的 Button（包括 `exit_button`）冲突都会在 analysis 阶段失败，不能覆盖 board 的输入 ID。参数属于 App 配置，不改变 board/skin 或共享入口的 lifecycle。

`//libs/lua/web:lua_web_app_argument_test` 验证预算、参数类型/长度/字符及 board 名冲突的校验函数。`//projects/example/targets/pkg_tar/lua-script-config:browser_test` 使用真实共享入口加载构建时生成的超过 128 KiB 的 Lua 文本，在显式 4 MiB VM 内保留 768 KiB 字符串，验证自定义参数和 Button ID，最后经页面 Stop 回收。该脚本是通用配置示例，不包含私有游戏；浏览器结果不代表设备内存余量或帧率。

`tools/bazel/web_archive.bzl` 的 `web_archive_browser_test()` 在 pinned Chromium（或 `H2_WEB_TEST_BROWSER`）中打开 archive：以用户手势点击 `#start`，收集 Console、异常与页面文本；全部 `passes` 正则出现即通过，`fails` 正则、`Aborted(`、`RuntimeError: `、未捕获异常或超时即失败。可选 `presses`（DOM 按键）、`taps`（Canvas 像素点击）、`clicks`（按 CSS selector 点击页面元素）、`evals`（在页面执行一段 JS，例如驱动输入边沿）、`canvas_min`（最少非黑像素）、`offline`（断网/恢复）与 `webrtc_server`（Pion fixture）。

| Target | 浏览器测试验证 |
|---|---|
| e2e `pal`（`?suite=browser`） | Memory/Time/Timer/Task/Queue/Mutex/Condition、IndexedDB Filesystem、Fetch HTTP、Netif、System Event 分发给 Runtime、raw Net 返回 `UNSUPPORTED`；teardown `fs=0 destroy=0` |
| e2e `libco` | 独立 libco Emscripten Fiber 库测试 |
| e2e `lua-runtime` | pthread 多 Worker；Lua 九 case |
| e2e `pal-core` | 41 个 Core 契约用例、真实栈和资源回收、无 yield 并行验证 |
| `display`、`qrcode`、`log` | Display 输出（非黑像素）、App 正常返回 |
| `lvgl-smoke` | LVGL 在 Web Task/Timer 上渲染，停止后 LVGL/Runtime 干净退出 |
| `touch` | Canvas 点击成为 Runtime Touch down/up（坐标一致），Enter 成为 Button down/up/action |
| `starboy` | 首帧 ready、持续动画、`should_stop` 退出 |
| `lua-cosmic-drift` | Lua 场景 ready、Canvas 点击、Escape → Back Button 取消 Lua job |
| `audio-system` | `--preload-file` 只读根上的 Opus 资源播放、fake 麦克风非静音 PCM 回环、worker join |
| `tap-reset` | LVGL App 在 Web task 中渲染、Canvas 点击、停止后 LVGL/Runtime 干净退出 |
| `lua-flappybird` | Canvas 点击、Escape → Back 取消并退出 |
| `lua-script` | `h2_lua_web_app()` + `demo_board` 默认 `device` 外观：canvas 与状态行进入外观的 slot；脚本校验 Button args 后 ready；点击外观的 `data-h2-button=ok` 元素触发脚本 OK 回调；点击页面 Stop（`#stop`）取消 job 并 PASS |
| `lua-script-input` | `demo_board` 的 `plain` 外观，Button 输入合同：pointer 与 Enter 重叠按住时松开 pointer 仍按住（只有一次 Down/Up）；blur、页面隐藏和 App 结束都释放按住的 Button；脚本只统计每次按压的首个 Down sample |
| `web-board-{amoled,bk7258,k4b,szp,p4-lcd43}` | 每块 web board：页面拿到板上的屏幕尺寸（`H2_WEB_BOARD_CHECK size=WxH`），`device` 外观放置 canvas，点击外观上的第一个 Button 到达脚本，点击 Stop 后 PASS |
| `web-hardware` | `prepare` 与 `configure_runtime` 在 App 前运行、Runtime `board` 为 hook 设置的名字；Enter 的 Down 与 Up 经 `button` 推到 component 42 / periph 501；`release_runtime` 关闭 configuration 持有的真实文件后 FS/platform 完整释放。额外 browser case 验证 configuration 失败后仍释放一次，以及 release 失败保留文件/backend、不重复释放 |
| `lua-script-stop` | 不按键，`run_ms` 发出 Stop 请求（`stage=stop-requested`），取消 job 后 PASS |
| `lua-script-extension` | extension 注册的 capability 可用；`exit_requested` 拒绝第一次 Escape、job 继续运行，第二次 Escape 取消并 PASS |
| `lua-script-vector` | 共享 Lua 入口绘制多边形、椭圆、硬边/平滑笔画和保留命令；唯一 native extension 经公开 Display API 创建／更新几何并重放缓存；真实 Canvas 检查边界、裁剪、变换、颜色与错误更新后旧数据，验证背景恢复、透明快照、retained 提交量、GC 后绘制和页面 Stop 回收 |
| `mp4-player`（manual） | WebCodecs H.264/AAC 播放完成；`:large_browser_test` 播放 1024×600 大文件；需 `H2_WEB_TEST_BROWSER` 指向 Google Chrome |

未提供 Web target 的 App：`gizclaw-ping-speed` 依赖必需的 Wi-Fi API；BLE、Wi-Fi CSI、modem、crash-before-confirm、partial-update 依赖浏览器不存在的硬件或板上能力；`lua-bloomspeaker` 依赖 BLE 配对；iperf 需要 raw socket。GizClaw 真实服务端注册与 H106 业务流程需要真实 token，不在自动测试范围内。

独立 `pal-core` Browser suite 验证 Core 与 System Event；`pal-wifi` Browser suite 验证 Netif 和真实 offline/online Runtime event；`pal-net-tls` 的 boundary test 验证 raw socket 不可用。`scripts/test/test-web.sh` 显式运行这些入口。Web provider 另有真实浏览器测试：

```sh
bazel test --config=macos_arm64 //libs/pal/providers/web/pal_core:browser_platform_test
bazel test --config=macos_arm64 --strategy=TestRunner=local --test_env=HOME \
  "--test_env=H2_WEB_TEST_BROWSER=/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  //libs/pal/providers/web/pal_core:browser_platform_test
```

第二条使用本机 Google Chrome 以覆盖 H.264/AAC 解码。

## Raster2D example

`//projects/example/targets/pkg_tar/raster2d:serve` 运行唯一无 Lua 的 C/WASM 示例，展示 RGB565 颜色端点、中间值、裁剪和覆盖。`:browser_test` 检查完整 240×240 Canvas。它直接运行 portable App，并复用 Web PAL Canvas backend。现有 `//projects/example/targets/pkg_tar/lua-script-vector:browser_test` 继续承担 Lua/C/WASM 显示回归；新增 binding 合同由 `//libs/lua:lua_test` 的真实 Host 测试覆盖。

Web 的同步 SystemEvent PAL API 只借给 C Worker，浏览器主线程 accessor 返回 `NULL`。浏览器发起退订时使用 `h2_web_platform_system_event_unsubscribe_async`：成功后立即停止新回调并将 subscription 所有权交给 platform Worker；完成回调仅在所有在途 handler 返回后执行，调用方此前必须保留 handler context。主线程入队使用 try-lock，竞争返回 BUSY 并交还所有权，不能同步阻塞浏览器等待 Worker。回调自身不能等待自己的异步退订完成。

## 可选的 GizClaw 固件控制 SDK

@gizclaw/firmware 只由需要控制 GizClaw 固件 HTML 的宿主网页引入。固件页面本身和只展示页面的 iframe 不依赖这个 npm 包，继续按产品原有入口、Start、权限和关闭流程运行。SDK 连接现有 iframe，不加载裸 WASM，也不启动或销毁产品。

```html
<iframe id="firmware" src="https://firmware.example/index.html"
        allow="cross-origin-isolated"></iframe>
```

```js
import { connectFirmware } from '@gizclaw/firmware';

const frame = document.querySelector('#firmware');
const api = await connectFirmware(frame);
const tools = await api.tools.list();
const state = await api.mhs.read({id: 'speaker.main', hwd: 'speaker'});
await api.mhs.write({
  id: 'speaker.main', hwd: 'speaker',
  payload: {volume_percent: 0, muted: false},
});
await api.tools.invoke('lua.app.install', {url: archiveUrl});
api.close();
```

H106 等产品入口只通过 C 注册已有 Service：先 h2_gizclaw_local_start()，再用 h2_web_gizclaw_open() 打开 //libs/gizclaw/web 的统一通信桥；入口不引用 npm SDK、对象 codec 或 SDK JS 文件。固件只接收 canonical method 与字节请求，所有工具和 MHS 仍走已有 handler。产品先 close C adapter，再走原有 Service stop/poll/deinit；SDK client.close() 只断开宿主连接，固件继续运行。

projects/gizclaw/libs/web 拥有 npm 控制端源码、官方 codec/map 和类型声明，projects/gizclaw/targets/npm_package/firmware 拥有 package identity、文件集合与 tarball。固件与 SDK 共用 ABI identity/version。窗口握手只接受嵌入它的 parent，SDK 检查 iframe window 和精确 origin，再建立 MessageChannel；不同 frame 的状态相互独立。同一 frame 新连接会关闭旧连接并取消其 pending 请求。origin 默认从 iframe.src 推导；发生跨 origin redirect 时由宿主显式提供最终 origin，opaque origin 不支持。SDK 等待已注册端点，不改变固件启动或浏览器授权。

对象方法与类型使用 pinned GizClaw codec/map。实例与可用工具取自产品原有注册表；lua.app.run 是否可用仍取决于产品 prepare hook。安装格式、URL/Binary 支持与交接语义仍遵守 [GizClaw Lua App](/zh/developing/gizclaw#lua-app)。SDK 不另外提供 Lua catalog、安装器或生命周期；call(method, Uint8Array, options?) 保留 canonical 字节接口。

调用返回 Promise，支持 {signal?, timeout_ms?}。请求在 transfer 前复制，回复在复制/解码后才确认 delivery completion；error.code 保留实际 signed PAL result，wire-only rejection 使用 -1000 并保留 rpcCode/rpcMessage，0/false 保留 optional presence。请求容量、请求字节限制与默认 deadline 来自固件配置。取消、断连或超时不能撤销已经发生的写入；解码失败不会提交准备好的工具 continuation。SDK close、iframe navigation 和宿主 pagehide 拒绝 pending 请求；产品 C failed join 仍保留依赖供重试。

多线程固件仍需要真实的 COOP/COEP hosting headers，跨 origin iframe 需要允许 cross-origin-isolated，固件响应需显式允许跨域嵌入（Cross-Origin-Resource-Policy: cross-origin）；这是现有 Web 固件的运行条件，npm 控制端不代替 hosting 或权限配置。//projects/gizclaw/targets/npm_package/firmware:release_tarball 生成独立 npm tarball。libs/gizclaw/web/tests/local/ 验证无 SDK 的固件运行；npm package 的 tests/embed/ 从实际 tarball 导入 SDK，控制不同 origin 的真实 WASM iframe，并覆盖现有 tool/MHS、Lua install/list/uninstall、错误、ACK、取消、重连和 close retry。测试 HTTP/Filesystem PAL 不代表真实服务器、Trial/H106 产品或物理设备验收。
