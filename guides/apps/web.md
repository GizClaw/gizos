# Web

Web 入口归 portable App 的 project owner。Web 不是 Mobile 子平台；它有独立的 lifecycle、toolchain 和 hosting contract。

## Structure

```text
projects/example/
├── libs/web/tap-reset/            # Web presentation 与 App contract conversion
├── targets/pkg_tar/tap-reset/     # Emscripten entry、HTML shell 和最终 Web archive
└── targets/pkg_tar/lua-flappybird/ # 同一 Lua Flappy Bird App 的 Canvas archive

projects/e2e/targets/pkg_tar/lua-runtime/ # Lua Runtime 九 case Browser archive

projects/h2loader/
├── libs/web/                      # 可复用的 H2Loader JS/runtime/WASM build target
└── targets/npm_package/h2loader/  # @gizclaw/h2loader Browser SDK npm package

libs/pal/providers/web/pal_core/    # Canvas/Emscripten reusable PAL backend
```

`projects/<owner>/libs/web/<app>` 只能保存 Web-specific wrapper、required capability 和 portable App contract conversion；`targets/pkg_tar/<app>` 负责 lifecycle、Runtime assembly、HTML shell、package metadata 和最终可交付 archive。Canvas display、pointer handler、Memory、Time 和 Queue backend 属于 `libs/pal/providers/web/pal_core`，不能复制进 project entry。

## Build Boundary

Web wrapper C source 由 `//projects/example/libs/web/tap-reset:tap_reset_web` 提供 Bazel ownership。`@emsdk` 的 `wasm_cc_binary` 用真实 Emscripten toolchain 编译 App 与依赖，`@rules_pkg` 的 `pkg_tar` 再把 `index.html`、`index.js` 和 `index.wasm` 打成 serve-ready archive：

```sh
make test-web
```

`make test-web` 构建 Tap Reset 以及 `projects/e2e/targets/pkg_tar/` 下的 Web archive，验证归档根目录、入口引用和 WASM magic，并通过 Emscripten/Node 执行 Web PAL、libco、portable PAL registry 与 fake Web Serial E2E。需要浏览器调试时，解包后使用任意静态文件服务托管目录；仓库不维护专用 runner：

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

当前 Web component 实现 Memory、Log、Time、Timer、Task、Queue、Sync、Display、Touch、Host Serial，Fetch HTTP、browser-seeded Crypto、Web Audio playback、WebCodecs H264/AAC decoding，以及基于浏览器 `RTCPeerConnection`/`RTCDataChannel` 的 WebRTC 信令、DataChannel 和 caller-owned audio track。Media 由调用方拥有：页面把 `{stream: MediaStream, audio: HTMLMediaElement}` 注册进 `Module.h2WebRtcTracks`（以非零 wasm32 整数 token 为 key 的 Map），再用 `native_handle` 等于该 token 的 `h2_pal_webrtc_track_t` 调用 `h2_pal_webrtc_peer_set_track()`；`stream` 和 `audio` 可以省略其中之一，但不能都省略。Provider 不执行 `getUserMedia`、不构造 `Audio`、也不调用 `MediaStreamTrack.stop`；浏览器仍完成 Opus/RTP 编解码。调用方必须让 JS 对象、registry entry 和 C Track 存活到 `h2_pal_webrtc_peer_unset_track()` 成功或 peer 关闭；unset 等待 `replaceTrack(null)` 完成并清理远端播放。页面必须从用户手势启动首次连接，以满足 microphone permission。

Web 的 raw Opus compatibility API 仍返回 `H2_PAL_ERR_UNSUPPORTED`；标准 `RTCPeerConnection` 不提供等价的 encoded-frame 注入/提取，Web App 不能回退到该接口。Task 与同步等待由 libco 的 Emscripten Fiber backend 在单个浏览器线程中协作调度；它们不承诺抢占或 CPU 并行。Web Serial 的异步 Promise 只记录完成，并请求一个不会嵌套进活动 Asyncify export 的后续 bounded platform pump；task deadline 和 Timer deadline 也由 platform 自动安排最早的后续 pump，entry 不需要依靠无关 UI 事件推进等待任务。端口授权必须直接来自用户手势。不具备浏览器 API 对应能力的控制线读取、network、microphone audio HAL、BLE 和其余 PAL 使用 canonical unsupported provider 返回 `H2_PAL_ERR_UNSUPPORTED`，不能伪造成功或平台身份。

生产 Web App 必须定义 hosting headers、browser lifecycle、permissions、release packaging 和 supported-browser acceptance，不能把 smoke page 成功运行当作 Web 平台完成证据。持有 Runtime 的 Web entry 在 `pagehide`/freeze shutdown handler 返回前必须同步拒绝新操作、请求 task cancellation、使 pending Serial I/O 以 `CLOSED` 退出，并执行 bounded pump 直到活动 PAL 调用退出，再依次 join task、deinit Runtime 和销毁 platform。Web Serial 仅提供 Promise 形式的 reader/writer cancellation 和 port close；活动 session 的页面生命周期 shutdown 必须返回 `UNSUPPORTED`，只能同步失效回调并发起 best-effort 浏览器清理，不能声称这些 Promise 在 handler 返回前完成。需要确定性关闭证据的产品必须在页面仍可推进 event loop 时提供显式、可等待的 close 流程。
