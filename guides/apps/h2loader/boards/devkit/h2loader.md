# DevKit H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/devkit:package
```

USB Serial/JTAG 是 canonical console；provider 只接受 launcher 声明的 allowlisted build variables，不能用文档暴露第二套 native build tree。内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 managed package 和 release metadata；ESP-IDF app descriptor 和 package manifest 使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

## Partition layout

待补充 H2Loader image 使用的 partition table 和各分区用途。

## sdkconfig

待补充 USB Serial/JTAG 与 UART console 对应的 `sdkconfig` defaults 和关键配置。

## 预期表现

先运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan`，只选择结构化 identity 为 `board=devkit`、`target=esp32s3`、`active_role=h2loader`、`transport=iostreamikcp` 的设备。随后用 scan 返回的 port 执行 `status`，确认 `upgrade_phase=idle`，再使用 `send --file <build-dir>/update.tar.zlib` 和 `upgrade` 完成 Loader self-update。

`H2_LOADER_UPGRADE result=OK` 只表示升级已被接受。验收还必须等待 trial 与 canonical 重连，重新执行 `status`，确认 active version 等于构建版本、运行 canonical partition 且 `upgrade_phase=idle`，并在 power-cycle 后再次确认。已经安装 H2Loader 的正常路径不直接烧录；只有 self-update 无法执行、重新确认 board/target identity 且已获得 destructive recovery 授权时，才按恢复流程使用本 target 的 `.recovery.h2fb`。
