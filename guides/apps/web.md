# Web

Web 入口归 portable App 的 project owner。Web 不是 Mobile 子平台；它有独立的 lifecycle、toolchain 和 hosting contract。

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
libs/app_host/                      # App 启动层（当前仅 Web）：launcher、HTML shell 与 h2_web_app() 宏
libs/lua/web/                       # 单个 Lua 脚本页面的 h2_lua_web_app() 宏与通用入口
```

`projects/<owner>/libs/web/<app>` 只能保存 Web-specific wrapper、required capability 和 portable App contract conversion；`targets/pkg_tar/<app>` 负责 lifecycle、Runtime assembly、HTML shell、package metadata 和最终可交付 archive。Canvas display、pointer handler、Memory、Time 和 Queue backend 属于 `libs/pal/providers/web/pal_core`，不能复制进 project entry。可复用的 App 启动层是 `libs/app_host`（见[目录结构](/zh/developing/repo_layout)）：它消费 `web/pal_core` 组装 Runtime，但不选择 App；任何 project（包括下游仓库）的 `pkg_tar` entry 都可以用它运行自己的 App。

## Build Boundary

Web wrapper C source 由 `//projects/example/libs/web/tap-reset:tap_reset_web` 提供 Bazel ownership。`@emsdk` 的 `wasm_cc_binary` 用真实 Emscripten toolchain 编译 App 与依赖，`@rules_pkg` 的 `pkg_tar` 再把 `index.html`、`index.js` 和 `index.wasm` 打成 serve-ready archive：

```sh
make test-web
```

`make test-web` 构建 `projects/example/targets/pkg_tar/` 与 `projects/e2e/targets/pkg_tar/` 下的 Web archive，验证归档根目录、入口引用和 WASM magic，通过 Emscripten/Node 执行 Web PAL、libco、portable PAL registry 与 fake Web Serial E2E，并在真实 Chromium 中运行各 archive 的 `:browser_test`（见下文 [Example 与 E2E 的 Web target](#example-与-e2e-的-web-target)）。每个 target 的 `:serve` 可在本机托管 archive。需要浏览器调试时，解包后使用任意静态文件服务托管目录；仓库不维护专用 runner：

```sh
mkdir -p build/web/tap-reset
tar -xf bazel-bin/projects/example/targets/pkg_tar/tap-reset/tap-reset.web.tar \
  -C build/web/tap-reset
python3 -m http.server 8000 --directory build/web/tap-reset
```

H2Loader Serial Web E2E 的 archive 是 `bazel-bin/projects/e2e/targets/pkg_tar/h2loader-serial/h2loader-serial.web.tar`。页面必须由用户点击按钮调用 Web Serial chooser；授权完成后，portable App 才能使用 opaque port ID。真实 status、只读 command 和 managed install 继续走同一 Host Core，install 由 launcher 提供精确 catalog SHA 与资源读取器。

[H2Loader Web SDK](./h2loader/apps/batch_loader/) 由 `//projects/h2loader/libs/web:h2loader_web` 提供 public JS module、matching Emscripten runtime 和 WASM，并通过 Web PAL/Host Core 执行 Web Serial、package inspection 和 managed lifecycle。`//projects/h2loader/targets/npm_package/h2loader:h2loader` 使用提交的 `package.json` 将完全相同的 SDK outputs 组装成 `@gizclaw/h2loader`；npm 是 Browser SDK 的分发格式，不提供 Node.js serial-port runtime。独立 workflow 在相关改动进入 `main` 后只发布尚不存在的版本。产品 frontend、hosting headers 和浏览器 UI 验收由 `GizClaw/www` 负责。浏览器保留 `SerialPort` 与 `File` 对象，C 只接收 runtime-scoped opaque handle 和 bounded slice。现有 H2Loader Serial Web E2E 继续验证底层协议与平台，不是产品 UI。

浏览器不能从本地 `file:` URL 加载生成的 `.wasm`。

Lua Flappy Bird archive 在 Canvas 上运行与 Desktop/AMOLED 相同的 portable App 和
ported Lua bytes。Web Task/Timer/Queue/Sync 让 Host worker 与 Lua coroutine 在单个
浏览器线程中协作推进；`lua-runtime` 必须报告 `scheduler=cooperative`，不能把多个
coroutine 或多个 job 描述成 Wasm pthread/SMP 并行。HTML 不增加 Back/Stop 控件；
浏览器把 Escape 键通过 `h2_runtime_button_push_edge()` 写入映射后的 Runtime Button
edge，由 portable App 作为唯一 Runtime Event consumer 识别并取消 job，HTML 不直接
修改 Lua/Host 状态。

仍使用 LVGL 的其他 Web App 与 Mobile 继续共享 `libs/lvgl:single_thread_config`。Web 的真实 Emscripten toolchain 需要 POSIX `strnlen` declaration，因此 `libs/lvgl:lvgl_web` 只增加 `_POSIX_C_SOURCE=200809L`，不复制配置或上游源码。

## Platform Boundary

当前 Web component 实现 Memory、Log、Time、Timer、Task、Queue、Sync、Display、Touch、Host Serial，Fetch HTTP、browser-seeded Crypto、Web Audio playback 与 microphone capture、WebCodecs H264/AAC decoding、基于 `navigator.onLine` 的 Netif 与 System Event、IndexedDB 持久 Filesystem（`h2_web_fs.h`），以及基于浏览器 `RTCPeerConnection`/`RTCDataChannel` 的 WebRTC 信令、DataChannel 和 caller-owned audio track。能力矩阵、错误语义与接入方式见下文 [Browser Runtime 能力](#browser-runtime-能力)。Media 由调用方拥有：页面把 `{stream: MediaStream, audio: HTMLMediaElement}` 注册进 `Module.h2WebRtcTracks`（以非零 wasm32 整数 token 为 key 的 Map），再用 `native_handle` 等于该 token 的 `h2_pal_webrtc_track_t` 调用 `h2_pal_webrtc_peer_set_track()`；`stream` 和 `audio` 可以省略其中之一，但不能都省略。Provider 不执行 `getUserMedia`、不构造 `Audio`、也不调用 `MediaStreamTrack.stop`；浏览器仍完成 Opus/RTP 编解码。调用方必须让 JS 对象、registry entry 和 C Track 存活到 `h2_pal_webrtc_peer_unset_track()` 成功或 peer 关闭；unset 等待 `replaceTrack(null)` 完成并清理远端播放。页面必须从用户手势启动首次连接，以满足 microphone permission。

Audio PAL 的麦克风通过 `getUserMedia` 与 `AudioWorklet` 采集，显式请求 `echoCancellation: true`，并在 Console 输出 track `getSettings()` 返回的实际状态；浏览器未确认启用时输出 warning，但仍允许采集。该参数是偏好，不能把请求成功视为实际消回声效果验收。采集由独立 16 kHz AudioContext 完成输入设备重采样，输出 mono S16LE，每帧 320 samples（20 ms）。八个可回收 buffer 同时约束 worklet message 和读取队列，耗尽时丢弃新输入，不阻塞 audio rendering thread。`mic_read` 至少需要 640 bytes，成功时填写实际 format；空队列的零 timeout 返回 `WOULD_BLOCK`，有限等待返回 `TIMEOUT`，同一 platform 的并发 read 返回 `BUSY`。Task 中的等待通过 libco yield，root caller 通过 Asyncify 等待。

麦克风首次 start 需要用户手势、HTTPS/localhost、浏览器授权和允许 blob worklet module 的 CSP；start 最多等待 30 秒。API 缺失返回 `UNSUPPORTED`，授权拒绝或设备不可用返回 `UNAVAILABLE`，运行中设备结束返回 `CLOSED`。Stop 幂等，清空队列、停止 owned native tracks、关闭独立 AudioContext，使未完成 start/read 返回 `CLOSED`；延迟到达的授权 stream 也会立即停止。销毁 platform 前必须 stop/cancel 并让活动 PAL call 返回。已启动的 stream 以 platform wasm32 address 为 key 暴露于 `Module.h2WebMicrophoneStreams`；页面可以将该 borrowed stream 放入 caller-owned WebRTC track registry，实现共用采集，但必须先 unset WebRTC track，再 stop microphone。

`//libs/pal/providers/web/pal_core:mic_test` 验证授权失败、延迟授权取消、device ended、超时、停止唤醒与重新启动；`:mic_browser_test` 在真实 Chromium 中用 48 kHz 合成麦克风设备验证 getUserMedia、AudioWorklet 与 16 kHz PAL PCM。自动测试不代表用户物理麦克风的收音质量验收。

`h2_pal_webrtc_peer_send_opus()` 仍返回 `H2_PAL_ERR_UNSUPPORTED`。`native_handle == NULL` 且带 read/write vtable 的 Opus Track（GizClaw 使用的 native provider 模型）通过 encoded transform 运行：provider 发送一条静音浏览器 track，把每个外发 encoded payload 替换为 `read()` 取得的 Opus packet，并把收到的 payload 交给 `write()`，不经浏览器解码播放；优先使用 `RTCRtpScriptTransform`（worker，CSP 需允许 `blob:` worker），缺失时使用 Chromium `createEncodedStreams`，两者都没有时返回 `UNSUPPORTED`。每个外发浏览器帧（20 ms packet time）替换为一个 Track packet，RTP timestamp 由浏览器生成，因此 Track 必须产生 20 ms Opus packet；read/write 只在 platform 拥有的 media task 中调用；收到的丢包不产生 zero-length marker。静音源依赖已解锁的 `Module.h2WebAudioContext`，页面须在用户手势中创建或恢复它；AudioContext 未运行时 set_track 在 Console 警告，上行在其恢复前没有帧。Task 与同步等待由 libco 的 Emscripten Fiber backend 在单个浏览器线程中协作调度；它们不承诺抢占或 CPU 并行。Web Serial、Fetch、WebRTC、WebCodecs、IndexedDB 与 Web Locks 的异步 Promise 都只记录完成，并请求一个不会嵌套进活动 Asyncify export 的后续 bounded platform pump；在 task 中等待这些 Promise 的 PAL 调用通过 libco 让出，其它 task、Timer 和 root 继续运行，只有 root caller 通过 Asyncify 等待。task deadline 和 Timer deadline 也由 platform 自动安排最早的后续 pump，entry 不需要依靠无关 UI 事件推进等待任务。端口授权必须直接来自用户手势。不具备浏览器 API 对应能力的控制线读取、raw socket Net、BLE 和其余 PAL 使用 canonical unsupported provider 返回 `H2_PAL_ERR_UNSUPPORTED`，不能伪造成功或平台身份。

生产 Web App 必须定义 hosting headers、browser lifecycle、permissions、release packaging 和 supported-browser acceptance，不能把 smoke page 成功运行当作 Web 平台完成证据。持有 Runtime 的 Web entry 在 `pagehide`/freeze shutdown handler 返回前必须同步拒绝新操作、请求 task cancellation、使 pending Serial I/O 以 `CLOSED` 退出，并执行 bounded pump 直到活动 PAL 调用退出，再依次 join task、deinit Runtime 和销毁 platform。Web Serial 仅提供 Promise 形式的 reader/writer cancellation 和 port close；活动 session 的页面生命周期 shutdown 必须返回 `UNSUPPORTED`，只能同步失效回调并发起 best-effort 浏览器清理，不能声称这些 Promise 在 handler 返回前完成。需要确定性关闭证据的产品必须在页面仍可推进 event loop 时提供显式、可等待的 close 流程。

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
| Display | `h2_web_platform_display_api()` | 已实现 | `:browser_platform_test` display |
| Pref | `h2_web_platform_pref_api()`（localStorage） | 已实现 | `:pal_core_test` |
| raw Net socket、MQTT、Wi-Fi、蜂窝、BLE、NFC、真实电池、板上 OTA | canonical unsupported | 不支持，返回 `UNSUPPORTED` | — |

### 默认网络（Netif）

浏览器只暴露一个默认路径：NAME `browser`、kind `H2_PAL_NETIF_KIND_HOST`。在线时状态为 `UP | LINK_UP | DEFAULT_ROUTE`；离线时 DEFAULT ref 返回 `NOT_FOUND`，`browser` ref 仍可查询但只有 `UP`。浏览器不暴露本机 IP、网关、DNS、MTU、MAC，provider 从不设置 `HAS_IPV4/HAS_IPV6`，也不填写这些字段。其它 kind、名称和 ID 返回 `NOT_FOUND`，不模拟 Wi-Fi 或蜂窝接口；对 `browser` 调用 `get_dns_servers` 或 `set_default` 返回 `UNSUPPORTED`，因为解析与路由归浏览器所有。没有布尔型 `navigator.onLine` 时所有调用返回 `UNSUPPORTED`。

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

`//libs/app_host` 是通用 Web launcher：创建 platform、可选持久 Filesystem 与 LVGL platform，组装完整 Web Runtime（其余能力为 canonical unsupported），在 task 中初始化 Runtime、运行 App、deinit Runtime（Runtime 的 input task 只能从 task 中 join），再关闭 Filesystem 并销毁 platform。可选 `buttons` 描述 Runtime Button：`{component_id, key, name}`。输入全部由 shell 的 JavaScript 处理：`key`（DOM `KeyboardEvent.key`）和页面中所有 `data-h2-button="<name>"` 元素（鼠标、触摸）共同按住同一个 Button，合并成一个按下状态后才写 `h2_runtime_button_push_edge()` edge；页面失焦、隐藏或 App 结束时全部释放，click/long-press 仍由 Runtime 判定。`run_ms` 让无终止条件的 App 在测试中停止：先让 `should_stop` 为真，2 秒宽限后取消 App task，使其下一次 PAL 等待返回 `EXIT`。控制台与 `#status` 输出 `H2_WEB_APP name=<app> stage=running|ready|stop-requested|cancel` 与 `H2_WEB_APP name=<app> result=PASS rc=0 fs=0 destroy=0`；PASS 要求 App 返回 OK 且 Filesystem 关闭、platform 销毁都成功。新 target 只需 `main.c` 与 `h2_web_app()`，宏生成 `.web.tar`、`:serve` 与 `:browser_test`。

### Web board

浏览器页面跑在一块 web board 上，它是实体 board 在浏览器里的对应物。`//libs/app_host:web_board.bzl` 的 `h2_web_board()` 声明这块板提供的外设和外观：

- `display_width` / `display_height`：屏幕（canvas）像素尺寸；
- `buttons`：有序的 Button 名到键盘键（DOM `KeyboardEvent.key`，可为空）的映射，最多 8 个，第 i 个是 periph id i + 1；
- `skins`：外观名到 HTML 文件（可带 `<style>`）的映射，一块板可以有多套外观，`default_skin` 指定默认那套。

外观要做得和实物一样：`app_host` 把页面组件创建进外观的插槽——`data-h2-slot="display"` 放 Canvas（`#canvas`），`data-h2-slot="controls"` 放 Start/Stop（`#start`、`#stop`），`data-h2-slot="status"` 放状态行（`#status`），缺少的插槽放进 `<body>` 末尾的普通容器；外观中 `data-h2-button="<name>"` 的元素和该 Button 的键盘键共同驱动同一个 Button，按下时带 `data-pressed="true"`。`app_host` 自己不带 UI，shell 只有页面骨架和输入脚本。

组装可运行 target 时选 board 和其中一套外观：`h2_web_app(board = "...", skin = "...")`，`skin` 省略时用该板的 `default_skin`，板上没有 `default_skin`（包括没有外观）时用朴素的 `default_layout.html`；显式给出的 `skin` 必须是板上的外观，否则 analysis 阶段失败并列出该板的外观。`h2_web_app()` 据此生成 `h2_web_board`（`h2_web_board_t`，屏幕尺寸与 Button 表）编进页面，`h2_web_app_host_config_t` 的 Button 按名字引用板上的 Button（`key` 为 NULL 时用板上的键），`display_width/height` 为 0 时用板上的尺寸。不指定 board 时用 `//libs/app_host:default_board`（240×240、无 Button、朴素布局 `default_layout.html`），所以现有 example 页面不变。产品 board 与外观放在该 board 自己的目录旁（例如下游仓库的 `boards/<board>/web/`），同一块板上的所有 App 复用。

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
相同的停止请求（用于测试）。`//libs/app_host:web_board_argument_test` 覆盖 board 参数校验（屏幕尺寸、超过 8 个 Button、非法 Button/外观名、未知 `default_skin`），`//libs/lua/web:lua_web_app_argument_test` 覆盖 `run_ms` 范围。其余参数原样交给
`h2_web_app()`。每个 package 只能有一个 `h2_lua_web_app()`/`h2_web_app()`。
这些宏内部 label 都用 `Label()` 解析到 GizOS，下游仓库可以直接 load
`@gizos//libs/app_host:web_app.bzl` 与
`@gizos//libs/lua/web:lua_web_app.bzl`。

`tools/bazel/web_archive.bzl` 的 `web_archive_browser_test()` 在 pinned Chromium（或 `H2_WEB_TEST_BROWSER`）中打开 archive：以用户手势点击 `#start`，收集 Console、异常与页面文本；全部 `passes` 正则出现即通过，`fails` 正则、`Aborted(`、`RuntimeError: `、未捕获异常或超时即失败。可选 `presses`（DOM 按键）、`taps`（Canvas 像素点击）、`clicks`（按 CSS selector 点击页面元素）、`evals`（在页面执行一段 JS，例如驱动输入边沿）、`canvas_min`（最少非黑像素）、`offline`（断网/恢复）与 `webrtc_server`（Pion fixture）。

| Target | 浏览器测试验证 |
|---|---|
| e2e `pal`（`?suite=browser`） | Memory/Time/Timer/Task/Queue/Mutex/Condition、IndexedDB Filesystem、Fetch HTTP、Netif、System Event 分发给 Runtime、raw Net 返回 `UNSUPPORTED`；teardown `fs=0 destroy=0` |
| e2e `libco`、`lua-runtime` | Emscripten Fiber 调度；Lua 九 case |
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
| `lua-script-stop` | 不按键，`run_ms` 发出 Stop 请求（`stage=stop-requested`），取消 job 后 PASS |
| `lua-script-extension` | extension 注册的 capability 可用；`exit_requested` 拒绝第一次 Escape、job 继续运行，第二次 Escape 取消并 PASS |
| `mp4-player`（manual） | WebCodecs H.264/AAC 播放完成；`:large_browser_test` 播放 1024×600 大文件；需 `H2_WEB_TEST_BROWSER` 指向 Google Chrome |

未提供 Web target 的 App：`gizclaw-ping-speed` 依赖必需的 Wi-Fi API；BLE、Wi-Fi CSI、modem、crash-before-confirm、partial-update 依赖浏览器不存在的硬件或板上能力；`lua-bloomspeaker` 依赖 BLE 配对；iperf 需要 raw socket。GizClaw 真实服务端注册与 H106 业务流程需要真实 token，不在自动测试范围内。

`//projects/e2e/targets/pkg_tar/pal` 注入了 Netif 与 System Event。真实浏览器测试：

```sh
bazel test --config=macos_arm64 //libs/pal/providers/web/pal_core:browser_platform_test
bazel test --config=macos_arm64 --strategy=TestRunner=local --test_env=HOME \
  "--test_env=H2_WEB_TEST_BROWSER=/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  //libs/pal/providers/web/pal_core:browser_platform_test
```

第二条使用本机 Google Chrome 以覆盖 H.264/AAC 解码。
