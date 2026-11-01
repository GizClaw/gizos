# H202

H202 是无屏 BK7258 产品，板级支持位于 `boards/h202/bk7258/`。Board identity 为 `h202`，target 为 `bk7258`。硬件依据 H202 BK main V2.0 原理图和 Pin Multiplexing V2.0，GPIO 编号与板级 `include/h2_h202_board_config.h` 一致。

同一块板有一个固定 Loader 和一个由它安装的 MFG App：

| 入口 | Canonical package target | 角色 |
| --- | --- | --- |
| [H2Loader](./h2loader) | `//projects/h2loader/targets/h2loader_tar_zlib/loader/h202:package` | `role=h2loader`，管理、安装、恢复与回滚 |
| [MFG](./mfg) | `//projects/mfg/apps/h202:package` | `role=app`，语音引导的十步厂测 |

其他 MFG 入口是替换管理镜像的 Loader 角色；H202 的 MFG 是 App 角色，原因见 [MFG](./mfg#为什么是-app-角色)。

MP_C（AES-off）和 MP_J（AES-on）两种芯片共用同一 board、源码、identity 和版本，只在交付时选择普通或加密产物，见 [H2Loader](./h2loader#mp-c-与-mp-j)。

本文档不宣称任何 H202 GizOS 镜像已通过真机安装或厂测验收。
