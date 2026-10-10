# ESP-Mosaico / ESP32-S31

ESP-Mosaico 使用双核 RISC-V ESP32-S31；不能使用 ESP32-S3 Xtensa archive。GizOS 通过独立 `esp32s31` 平台选择 ESP-IDF 6.2 开发线的固定提交与 GCC 16.1.0，既有 S3/P4/C5 继续使用原来的 6.0.3 合同。

## SDK 与最小构建

SDK identity 位于 `tools/bazel/native_versions/esp_idf_s31_commit.txt`，工具合同位于 `esp_idf_s31_tool_versions.txt`。设置 `IDF_S31_PATH` 与 `IDF_S31_TOOLS_PATH`，SDK 必须为该固定提交的干净 checkout（含子模块），工具目录必须包含唯一的 `idf6.2_py*_env`。从该 checkout 执行 `install.sh esp32s31`；系统 Ninja 必须为 1.13.2。Bazel 不自动安装 SDK、不访问设备。

```sh
bazel test //boards/esp_mosaico/esp32s31:revision_test
bazel build --config=esp32s31 //projects/e2e/targets/esp_idf_firmware/reference-smoke/esp_mosaico:firmware
```

最小固件读取 eFuse 并输出板卡版本，然后运行 portable reference smoke。它不驱动供电 GPIO、不初始化 Runtime，也不代表外设或 H2Loader 已验收。该入口使用扩展接口 UART；主 Type-C 的运行期 CDC 尚需完整 board 启动路径提供。主 Type-C 的 ROM 下载功能不能证明应用有常驻串口。

## 板级验证固件

完整 Runtime 的独立诊断入口和烧录说明见
[board E2E](../../projects/e2e/targets/esp_idf_firmware/mosaico-board/esp_mosaico/README.md)。
该入口接入主 Type-C TinyUSB CDC，运行共享 PAL 基础 Core suite，并提供色条、
触摸/按键事件、IMU/磁力计和电量观测。这里的 CDC 是可选 board component；
前述最小 smoke 和当前 H2Loader 的 UART 通信入口不会因此自动切换为 USB。
可选 SC101IOT 摄像头采集和动态预览已完成 bring-up 验证；NAND 和完整外设资格测试仍未完成。

## 版本与上游

硬件依据为 [官方 V1.2 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html)、[V1.0 指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_v1.0.html) 和 [官方 BSP 固定提交](https://github.com/esp-mosaico/esp-mosaico-bsp/tree/a4bfb426afcf2911c01fdd9b6d6f024019f00f8f)。第三方 archive 由 GizOS 固定 SHA-256，`bsp` 与 `boot_splash` native component 由本 board 声明 ownership；Firmwares 不复制这些源码。

前 16 个 USER_DATA eFuse 位按 little-endian 解码。0x0100 使用 v1.0 映射，0x0101/0x0102 使用 v1.2 映射。未烧录、无法读取和未知版本均失败，不默认选择 v1.2。

| 信号 | v1.0 | v1.1 / v1.2 |
| --- | --- | --- |
| 主 I2C SDA / SCL | 0 / 1 | 56 / 3 |
| LCD RST / SCLK | 42 / 44 | 44 / 42 |
| codec 供电控制 | GPIO56，高有效 | 无 GPIO，常供电 |
| 状态灯 | GPIO3，低有效 | 无 |
| 扩展 I2C | 共享 I2C0 | 独立 I2C1，GPIO0 / 1 |

官方 BSP 的实际 manifest 和显示实现使用 CST9220；部分 README/注释仍写 CST9217。选择驱动应以上游实现及对应硬件 revision 为准，实机还需确认控制器识别与坐标方向。

## 验收边界

构建、启动通信、外设和安装更新是不同阶段。没有设备时，启动、CDC 重连、PSRAM/XIP 压力、跨核任务回收、供电电流、显示/触摸、音频、传感器、NAND 和更新回滚全部标记 SKIP；host test 或 Bazel query 不替代它们。构建目标不会自动访问或烧录设备。

后续 Firmwares 应更新 GizOS 固定提交，设置独立 S31 SDK locator，选择 `esp32s31` 工具链，消费 GizOS 公共 board/component 与 H2Loader layout，只保留产品入口和产品 task policy。Runtime 组合已有 V1.2 bring-up 证据；H2Loader 的编译与安装、更新、回滚实机验收分别记录，不能直接把 S3 产品 target 改名为 S31。

## 可选摄像头组件

最终固件 graph 选择 `//boards/esp_mosaico/esp32s31:camera`，在载入 native manifest、
设置 `EXTRA_COMPONENT_DIRS` 后、引入 ESP-IDF `project.cmake` 前包含
`boards/esp_mosaico/esp32s31/camera/sdk_setup.cmake`。该脚本只修改构建目录中的
`esp_driver_cam` 副本；SC101IOT 补丁只作用于临时项目的 managed component。
消费者无需复制 E2E 的补丁逻辑，也不能直接使用上游会修改共享 SDK 的 CMake。

组件通过 `h2_mosaico_camera()` 返回可选 PAL；调用前初始化官方模块管理器，
所有调用由一个消费者串行执行。固定左侧、1280×720 UYVY，获取超时 1000 ms；
不包含 Runtime camera 绑定、编码或联网服务。最终固件配置 DVP 和所需 sensor，
E2E 的 `sdkconfig.e2e.defaults` 是诊断配置示例。当前实机只验收 SC101IOT。

实现边界、资源契约和证据范围见 [适配指南](../../guides/zh/developing/esp_mosaico.md)。

S31 的 Bazel 库和 native SDK 必须统一使用 Picolibc：工具链用 `-specs=picolibc.specs` 探测并镜像系统头文件，编译时使用对应的 TLS/栈保护 ABI 参数，
板级 SDK defaults 显式选择 `CONFIG_LIBC_PICOLIBC=y`。不能将默认 Newlib 头文件构建的库
与 Picolibc 固件混用；Lua 字符分类和标准 IO 会暴露这种 ABI 不匹配。

### Type-C H2Loader transport

The Loader and E2E launchers opt into `esp32s31:loader_usb` before starting the
Runtime or command service. Its two CDC interfaces isolate diagnostic stdio
(CDC0) from reliable H2Loader IO Stream iKCP commands (CDC1). The board adapter
owns TinyUSB initialization and bounded physical I/O; it registers callbacks with
the public ESP H2Loader startup interface. The portable protocol and other boards'
UART/USB Serial-JTAG defaults are unchanged. Standalone board diagnostics may
continue using the single-CDC console without importing H2Loader.

Use the command interface for the host CLI and the diagnostic interface for E2E
ledgers. Verify physical USB identity and the authoritative H2Loader UID before
installing; do not select an unrelated USB-UART adapter. Real handshake, install,
upgrade and recovery qualification is tracked separately in the E2E matrix.
