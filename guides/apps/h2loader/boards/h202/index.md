# H202

H202 是无屏 BK7258 产品，板级支持位于 `boards/h202/bk7258/`。Board identity 为 `h202`，target 为 `bk7258`。硬件依据 H202 BK main V2.0 原理图和 Pin Multiplexing V2.0，GPIO 编号与板级 `include/h2_h202_board_config.h` 一致。

同一块板有两个互相替换的 Loader 角色镜像：管理用的 H2Loader 和厂测用的 MFG。

| 入口 | Canonical package target | 角色 |
| --- | --- | --- |
| [H2Loader](./h2loader) | `//projects/h2loader/targets/h2loader_tar_zlib/loader/h202:package` | `role=h2loader`，管理、安装、恢复与回滚 |
| [MFG](./mfg) | `//projects/mfg/apps/h202:package` | `role=h2loader`，仅 UART 管理，语音引导的十步厂测 |

与其他 MFG 一样，H202 MFG 替换管理镜像；为放进 Loader 窗口所做的取舍见 [MFG](./mfg#loader-角色与镜像大小)。

MP_C（AES-off）和 MP_J（AES-on）两种芯片共用同一 board、源码、identity 和版本，只在交付时选择普通或加密产物，见 [H2Loader](./h2loader#mp-c-与-mp-j)。

本文档不宣称任何 H202 GizOS 镜像已通过真机安装或厂测验收。
