# H2Loader Web SDK

GizOS 提供 `@gizclaw/h2loader` Browser SDK，负责 package inspection、Web Serial 授权端口和 H2Loader managed lifecycle。产品 Web UI 已迁移到 [`GizClaw/www`](https://github.com/GizClaw/www) 的 `/tools/` 页面；GizOS 不再维护 React/shadcn 前端、Batch Loader 静态 archive 或产品页面状态。

## Ownership

```text
projects/h2loader/libs/web/                       # JS wrapper、Emscripten runtime 与 WASM source target
projects/h2loader/targets/npm_package/h2loader/   # @gizclaw/h2loader package identity 与发布规则
libs/h2loader_host/                               # 协议、package 与 lifecycle authority
libs/pal/providers/web/pal_core/                  # Web Serial 与浏览器调度
GizClaw/www                                       # GizOS 网站、Batch Loader UI、状态和交互
```

依赖方向固定为产品 Web App → `@gizclaw/h2loader` → Host Core/Web PAL。产品 App 只调用 `createH2Loader()` 返回的 Promise API，不读取 Emscripten pointer、heap 或 `Module`。

## SDK lifecycle

SDK 支持恢复已授权端口、请求或撤销 Web Serial 授权、校验 format-1 package、读取 authoritative status，并执行 `stage`、`stageUrl`、`abortStage`、`rebootApp`、`rebootLoader` 与 `rebootUpgrade`。`SerialPort`、File 和 firmware bytes 只在浏览器 session 内存中存活；持久化产品 metadata、并发调度和页面展示由消费方拥有。

`status(port)` 与 `stage(port, blob)` 返回的 status object 包含无符号 32-bit `commandAvailability`，以及 active identity、`bootIntent`、`stage`、`partition1`、`partition2` 和 `lastResult`。命令位由 connected firmware 决定；present `0` 表示当前没有命令可用，不能从 static `capabilities` 或其他状态字段合成。该值只是 connected status snapshot，Host Core 与设备执行路径仍会 fail closed。

SDK 要求 secure browser context 和 Web Serial。确定性释放必须在 event loop 仍可推进时 `await loader.close()`；`pagehide` 只适合阻止新工作并发起 best-effort cleanup。

## Build and publish

```sh
bazel build //projects/h2loader/targets/npm_package/h2loader:h2loader
bazel run //projects/h2loader/targets/npm_package/h2loader:h2loader.publish -- --dry-run --ignore-scripts
```

`package.json` 是独立版本源。相关变更进入 `main` 后，发布 workflow 只发布 registry 中尚不存在的版本。npm package 仅支持 Browser/Web Serial，不提供 Node.js serial-port runtime。
