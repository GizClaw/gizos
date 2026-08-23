# 产品文档

产品文档面向基于 GizOS 开发的具体 App，以及被多个 App 共同使用的产品级集成。每个产品在这里记录产品流程、业务状态、required component、Runtime event/state 投影、资源生命周期、H2Loader package 和跨 board 验收要求。

App 文档不按 `zh`、`en` 拆分目录，统一存放在 `guides/apps/`，当前内容直接使用中文。

通用 Runtime、LVGL、PAL 和仓库结构属于[开发指引](/zh/guide)。H2Loader 本身是产品，其产品合同与工程结构统一放在本目录。

产品流程、页面与验收文档统一遵守[产品文档规范](/apps/documentation)。

## Product IA

Product IA 优先在 Figma 中组织产品页面、关键状态和跨模块关系，并由对应 Guide 链接到可审查的具体 page、section 或 frame。仓库内静态 IA 是可选发布形式；下表只登记已经交付的静态 IA，不要求新产品同时维护 Figma 与 HTML/SVG 两份视觉材料。具体规则见[产品文档规范](/apps/documentation)。

| Product App | Guide | IA |
| --- | --- | --- |
| H2Loader Batch Loader | [Batch Loader](/apps/h2loader/apps/batch_loader/) | [Figma](https://www.figma.com/design/cXBvKDzViDN8HJhKJq6IQn/H2Loader-%E2%80%94-Web-Batch-Loader?node-id=12-331) |

## 文档边界

| 文档 | 负责内容 |
| --- | --- |
| [开发指引](/zh/guide) | GizOS 项目本身，以及 Runtime、LVGL、PAL 等通用开发机制 |
| 产品文档 | H2Loader 等产品、具体 App 与跨 App 产品集成 |
| [审核指引](/zh/reviewing/) | Issue、实现和 Pull Request 的审查方法 |
| [编码规范](/zh/coding-styles/) | C、Go、Markdown 和命名规则 |
| [使用说明](/zh/using/) | 已有固件、H2Loader 和工具的使用方法 |

## 产品

- [H2Loader](/apps/h2loader/)：Host、Loader/App image、package 生命周期、board 支持矩阵与设备端诊断 App。
    - [H2Loader Batch Loader](/apps/h2loader/apps/batch_loader/)：在桌面浏览器中运行的 React/shadcn 批量烧录 App，通过内部 WASM SDK、已授权 Web Serial、本地 package、并发 managed install 和最终验证工作。
    - [BLE iKCP Baseline](/apps/h2loader/apps/bleikcp_speed/)：设备对设备的 BLE iKCP 吞吐、断线恢复和屏幕诊断合同。
- [GizClaw](/apps/gizclaw)：跨 App 的连接、请求状态、Audio System 和 OTA 集成合同。
- [Mobile](/apps/mobile)：真实 portable App 到 mobile contract 的 adapter，以及彼此独立的 iOS、Android launcher。
- [Web](/apps/web)：真实 portable App 到 Browser/WebAssembly contract 的 adapter、Bazel `pkg_tar` 交付物和 hosting gap。
- [Embedded Linux](/apps/embed_linux)：普通 Linux executable/service launcher、Bazel artifact 与标准 ADB 应用烧录边界。
- [E2E 测试 App](/apps/e2e)：可由 Desktop 与真实设备复用的 headless test registry、launcher ownership 与分层验收边界。
- [Examples](/apps/example)：target-independent runnable Example project group；包含 Audio System、BLE Broadcaster/Observer、BLE iKCP Baseline、BLE Provider、Display、GizClaw Ping Speed、Log、LVGL、Modem、MP4 Player、Tap Reset 与 Wi-Fi CSI。
- [Showcase 展架程序](/apps/showcase)：跨产品型号复用的待机视频、按住说话、前景角色对话和本地管理程序。
- [Tuxemon](/apps/pixa_games/tuxemon)：基于 Tuxemon 地图资源的 2D tile RPG 原型，支持大世界、室内、碰撞、传送和持续移动。
- [PIXA Games](/apps/pixa_games)：使用 Runtime input、Game Runtime、PixelRoot32 和 PIXA 实现的 reusable game library family。
    - [DinoRun](/apps/pixa_games/dinorun)：横向自动奔跑与 charge jump。
    - [DinoDive](/apps/pixa_games/dinodive)：平台下落、方向切换与 floor progression。
    - [DinoBounce](/apps/pixa_games/dinobounce)：paddle、fireball 与下降 brick。
    - [DinoTetris](/apps/pixa_games/dinotetris)：10 × 20 falling-block game。
