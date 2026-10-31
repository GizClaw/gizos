# ESP-Mosaico

ESP-Mosaico 使用 ESP32-S31、480×480 圆形触摸屏和板载 Type-C OTG CDC。 板级支持位于 `boards/esp_mosaico/esp32s31/`，沿用现有 16 MiB NOR `layouts/h2loader`，管理镜像容量 3 MiB、App 容量 7 MiB。

同一块板有两个可替换的管理镜像，二者均为 `role=h2loader`：

| 入口 | Canonical package target | 用途 |
| --- | --- | --- |
| [普通 H2Loader](./h2loader) | `//projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package` | 现有管理、安装与恢复 |
| [MFG](./mfg) | `//projects/mfg/apps/esp_mosaico:package` | 十二项共享验收；全部 PASSED 后才允许 App 安装和启动 |

MFG 替换管理镜像，无需另装一个 MFG App。普通 Loader 保持现有实现；两个入口 均复用板级 Runtime、USB console 和 Loader USB adapter。MFG 仅通过 USB 提供 管理命令，BLE 用于扫描验收，不启用 BLE 命令服务。

MFG 的黑橙测试页、中文标签、结果和重新测试控件由共享 MFG 提供。 Loader 的安装、失败和启动显示回调均为空；安装结果通过现有 CLI 命令、status 与运行镜像 identity 核对。此集成不包含品牌页或安装进度界面。

摄像头使用左 H2 的 SC101IOT，安装或拆卸必须先断电。板级 AEC、摄像头、USB 和磁力计语义见[开发说明](/zh/developing/esp_mosaico)。新 GizOS MFG 的构建容量、 真机十二项验收与管理链路仍需对准确 commit/package 记录证据；原 Firmwares 实现的历史结果仅是行为参考。
