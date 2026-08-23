# Embedded Linux

Embedded Linux entry 归 portable App 的 project owner，使用 `cc_binary/<app>/<board>` 组装普通 Linux executable、service unit 和可 staging 的安装树。它适用于已经运行 Linux userspace、能够执行普通 ELF 程序的设备，不是 H2Loader image，也不拥有操作系统或 rootfs。

## Ownership

```text
projects/example/targets/cc_binary/<app>/<board>/
├── BUILD.bazel
├── main/
└── package/
```

- `main/` 只组装 board runtime、target component 与 portable App entry。
- `package/` 保存 service unit 和安装 metadata。
- `BUILD.bazel` 定义 executable、target component、resource、service metadata 和 runtime library 的完整依赖。
- 私有 vendor SDK 由 shared development environment provision，再通过 Bazel local repository 接入；artifact entry 不读取任意 host path，也不保留第二套 Make build graph。
- Portable App、共享 PAL backend 与物理 BSP 分别继续属于 `projects/` 下的 App owner、顶层 `libs/pal/providers/linux/` 与 `boards/`。

只服务一个 project 的 Embedded Linux glue 放入该 project 的具名 `libs/<component>/`；可跨 project 复用的 PAL 实现属于顶层 `libs/pal/providers/linux/`。目录对称本身不是建立 component 层的理由。

## 应用烧录边界

标准 ADB 是与 H2Loader 并列的设备工具。对于 Embedded Linux target，ADB 自己负责设备发现、显式设备选择、文件传输、远程执行与状态查询；GizOS 不再包装另一套 `h2-adb` 或把设备操作放进 Bazel build rule。把 executable、资源或 service unit 写入设备的目标文件系统属于应用烧录。

以下事项是操作前置条件，不属于 App artifact entry：

- 设备已经安装可启动的 Linux OS/rootfs。
- 设备端已经运行可连接的 `adbd`，并按产品安全策略完成授权。
- 目标 ABI、动态加载器、共享库、写权限和 service manager 满足 App 要求。

GizOS shared development environment 提供固定版本的 ADB client；通用操作合同见 [Embedded Linux 使用说明](/zh/using/embed_linux/)。具体 board 必须另外提供使用页，写明 ABI、安装路径、service manager、资源冲突、验收 marker 和恢复步骤。

## 当前 entries

| Board | App | Build | Device operation |
| --- | --- | --- | --- |
| KICKPI K4B | Display smoke | Bazel | 标准 ADB；见 [K4B 使用页](/zh/using/embed_linux/kickpi_k4b) |
| KICKPI K4B | MP4 Player smoke | Bazel + CedarX H.264 + FDK-AAC + ALSA | 标准 ADB；见 [K4B 使用页](/zh/using/embed_linux/kickpi_k4b) |
| KICKPI K4B | Touch smoke | Bazel + Linux evdev Touch + LVGL + mapped Runtime Button | 标准 ADB；见 [K4B 使用页](/zh/using/embed_linux/kickpi_k4b) |
