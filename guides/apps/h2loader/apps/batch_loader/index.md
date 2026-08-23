# H2Loader Batch Loader

H2Loader Batch Loader 是工厂使用的浏览器批量安装工具。产品 UI 是 React JavaScript/JSX DOM App，使用提交到仓库的 shadcn component source、Tailwind CSS 和 Lucide；它不再维护 LVGL/Canvas 版本。

设计来源：

- [设备列表](https://www.figma.com/design/cXBvKDzViDN8HJhKJq6IQn/H2Loader-%E2%80%94-Web-Batch-Loader?node-id=12-331)
- [更换当前固件弹窗](https://www.figma.com/design/cXBvKDzViDN8HJhKJq6IQn/H2Loader-%E2%80%94-Web-Batch-Loader?node-id=19-685)

## Ownership

```text
projects/h2loader/apps/batch-loader/app/          # React/shadcn product UI and batch controller
projects/h2loader/libs/web/                       # reusable internal JS/runtime/WASM SDK target
projects/h2loader/targets/pkg_tar/batch-loader/   # the only deployable archive
libs/h2loader_host/                               # protocol, package and lifecycle authority
libs/pal/providers/web/pal_core/                   # Web Serial and browser scheduling
```

依赖方向固定为 Web App → `//projects/h2loader/libs/web:h2loader_web` → Host Core/Web PAL。App 只调用 `createH2Loader()` 返回的 Promise API，不读取 Emscripten pointer、heap 或 `Module`。SDK 是 Bazel build target，不发布 npm package、独立 SDK archive 或 release asset。

## UI 与操作

页面以 Devices table 为主体，不提供 filter、固定槽位或设备数量上限。`Add device` 必须从直接 click stack 调用 Web Serial chooser；已授权端口在启动时恢复并追加到表格。Scan、Add device、Rollback、Restart、Cancel 和 Flash selected 全部位于 Devices card header；表格底部的 `Authorize port` 与页头 `Connect device` 复用同一个 chooser 入口。表格列为 Device、Board、Current image、Status、Progress / detail 和 Action：表头按钮(Scan / Switch to Loader / Switch to App / Restart / Send package)对**选中**的设备批量执行,按每台扫描回来的 role+capabilities 决定是否可用;每行右侧 `···` 菜单对**单台**执行同样的操作。所有行的 `···` 菜单都提供 `Forget`：在线行先通过 SDK `forgetPort()`（Web Serial `SerialPort.forget()`）撤销浏览器授权，再删除该行与本地 metadata，离线历史行只删除本地 metadata。页头的 GizOS Marketplace 导航仅是设计稿中的壳层，不承载 Batch Loader 功能。`Add device`/`Connect device`/`Authorize port` 授权成功后立即对新行执行一次 `status()`，把 Board、Current image 和状态读出来，不必再手动 Scan。

界面文案使用 i18next + react-i18next：资源在 `src/locales/{en,zh-CN}.json`，语言由 `localStorage`（`h2loader.batch-loader.language`）→ 浏览器语言检测，默认 en，页头提供 EN/中文 切换并同步 `<html lang>`。SDK 的结构化错误文本不属于翻译合同。

Current package 打开独立弹窗。操作员选择 APP 或 Loader role 后，通过 browse 或 drop 选择本地 format-1 `*.update.tar.zlib`；SDK 先校验 package 并展示 role、board、target、version 和 size，只有 role 匹配且确认后才替换当前 package。File 与固件 bytes 只存在于当前页面 session。

非扫描操作最多同时 claim 四台设备，超出部分保持 queued。每台设备有独立 phase、progress 和 terminal result；单台失败或取消不能覆盖其他设备结果。Install 只有经过 stage、activate、同一授权 port 重发现、reconnect 与 Host Core final verification 后才显示成功。Rollback 和 Restart 的 command accepted 结果显示为 stale，直到下一次 Scan。

`Send & Switch`(传包+切换)是工厂主流程的绿色主操作:完整 managed install = 传包 + 切换为应用(loader 安装暂存 app 并重启)+ 校验,一步完成;install 的 activate 是慢安装,读取超时给到 120s。`Send package`(传包)是普通色的分步操作,只把固件包 stage 到 Loader 的暂存区(需设备处于 Loader 模式),**不重启设备**:传输 100% 且校验 staged 一致即成功,随后由 `Switch to App`(切换为应用)让 Loader 解压并启动。它使用 Host Core 的 `h2_h2loader_host_stage_operation_run`(stage → 重连读状态 → 校验 staged),不做 activate/reboot,因此不会出现完整 managed install 里 activate 重启后重连/校验超时的情况。

每行 `···` 菜单的「查看状态」打开设备状态弹窗:把 status 字段转成可读分区——应用可用性(已安装可启动 / 已暂存待安装 / 无可用应用 / 应用运行中 / 未扫描)、固件身份与版本、以及 8 个 capability 的启用情况。未扫描的设备显示提示而不是编造数值。设备自身的 install state 以 `installState` 保存,避免与表格行的 UI state 冲突。表格使用固定列宽(`table-fixed`),状态文案变化不会导致列宽跳动。

调度以**单台设备**为单位互斥:串口一次只能服务一个操作,所以忙碌的是设备而不是界面。任意来源(表头批量 / 行菜单单台)发起的**写操作**共享同一个并发预算(最多 4);`status` 扫描只读状态,不受该上限限制,可覆盖整个已授权列表,彼此独立运行——一台失败或取消不影响其它台。忙碌设备的勾选框与行内操作禁用,批量按钮在**选中项中存在忙碌设备**时禁用。`Cancel` 不依赖勾选:表头 Cancel 取消所有正在运行/排队的设备,行菜单的取消只停这一台。

## Persistence 与 lifecycle

localStorage 只保存 versioned operator metadata：显示名、顺序、选择状态和上次显示信息。 设备身份使用 SDK 暴露的稳定端口 id（Web PAL 的 `web-serial-N`，与旧 LVGL 版一致）：同一会话内每个已授权端口有唯一稳定 id，即使两台设备 USB VID/PID 相同也各自独立，绝不塌缩或串行。表格在设备名下显示 `USB <VID>:<PID> · #<设备序号>` 便于核对;SDK 的 port handle 保持私有,页面身份由 live port 对象的 WeakMap 序号派生。 设备默认名用稳定编号 `Device N`（N 取自 `web-serial-N`），不再使用 Web Serial 的空占位名，操作员可改名覆盖。跨刷新时 `web-serial-N` 会重新编号，持久化的显示名按 VID/PID 做 best-effort 一一重绑定；同型号（相同 VID/PID、Web Serial 不暴露序列号）无法保证显示名一定落回同一物理口，必要时由操作员重新命名。`SerialPort`、SDK handle、File、固件 bytes 和 authoritative Ready 不持久化。重载后的历史信息一律是 stale；破坏性操作仍依赖 live SDK object 与新的 authoritative status。

每批操作使用 generation fence 和 `AbortSignal`。Cancel 停止新 claim，并在 bounded SDK/transport boundary 请求活动操作取消。`pagehide` 立即阻止后续 App work 并启动 best-effort close；需要确定性释放时必须在 event loop 仍可运行时 await `close()`。

## 构建、测试与部署

```sh
bazel test //projects/h2loader/apps/batch-loader/app:playwright_test
bazel build //projects/h2loader/targets/pkg_tar/batch-loader:batch-loader
bazel run //projects/h2loader/targets/pkg_tar/batch-loader:serve -- \
  --host 127.0.0.1 --port 8000
```

唯一发布产物是 `batch-loader.web.tar`。archive 根目录包含 DOM frontend assets、`sdk/h2loader.js`、matching Emscripten runtime/WASM 和 `_headers`；不包含 Canvas shell、LVGL asset、`index.data`、nested SDK archive 或 package metadata。生产环境必须使用 HTTPS，并提供 `application/wasm`、`Permissions-Policy: serial=(self)` 及 archive 中等价的 CSP/security headers。

Bazel Playwright target 使用 hermetic Chromium 和 Bazel-managed static server。自动测试可以验证 DOM、调度、持久化和 real JS→WASM SDK loading；真实 USB、Windows driver、Chrome/Edge compatibility 和五台设备的四 active/一 queued 工厂验收仍需人工记录。
