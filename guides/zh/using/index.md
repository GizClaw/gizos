# 使用说明

使用说明面向 GizOS 提供的完整系统和工具，介绍如何使用、操作和调试这些能力。

## H2Loader

H2Loader 使用说明包括以下内容：

- [总览与环境](./h2loader/)：选择工厂 Batch Loader 或 repository-only CLI、配置 operation env 和检查依赖。
- [CLI](./h2loader/cli)：H2Loader CLI 的使用方法。

H2Loader 的产品合同、board/image 配置、代码结构与固件开发约定见 [H2Loader 产品文档](/apps/h2loader/)。

## Embedded Linux

已经运行 Linux userspace 与 `adbd` 的设备通过标准 ADB 执行应用烧录：

- [通用 ADB 流程](./embed_linux/)：准备 ADB、scan、显式设备选择、预检、临时运行、持久化安装与恢复。
- [KICKPI K4B](./embed_linux/kickpi_k4b)：ARMv7 hard-float ABI、Display/MP4 Player framebuffer 互斥、CedarX runtime libraries、安装路径与验收 marker。

该流程只传输和运行普通 Linux executable、资源及 service metadata，不安装操作系统或 rootfs。
