# H202

H202 是无屏 BK7258 产品，板级支持位于 `boards/haivivi/h202_bk_v3/bk7258/`。Board identity 为 `h202_bk_v3`，target 为 `bk7258`。The H202-BK V3.0 column in the 2026-11-27 pin multiplexing sheet assigns GPIO14–19 to SDIO and GPIO45 to card detect. GPIO22–27 are unpopulated QSPI pads. The V2.0 4G assignments (GPIO5, 7, 13, 40, 41, 45, and 55) are removed or reassigned.

同一块板有两个互相替换的 Loader 角色镜像：管理用的 H2Loader 和厂测用的 MFG。

| 入口 | Canonical package target | 角色 |
| --- | --- | --- |
| [H2Loader](./h2loader.md) | `//projects/h2loader/targets/h2_loader_tar_zlib/loader/haivivi/h202_bk_v3:package` | `role=h2loader`，管理、安装、恢复与回滚 |
| [MFG](./mfg.md) | `//projects/h2loader/targets/h2_loader_tar_zlib/mfg/haivivi/h202_bk_v3:package` | `role=h2loader`，仅 UART 管理，语音引导的十一项厂测 |

与其他 MFG 一样，H202 MFG 替换管理镜像；为放进 Loader 窗口所做的取舍见 [MFG](./mfg.md#loader-角色与镜像大小)。

Both entries use the standard BK7258 bootloader and ordinary `:package` artifacts. See [installation and recovery](./h2loader.md#installation-and-recovery).

本文档不宣称任何 H202 GizOS 镜像已通过真机安装或厂测验收。
