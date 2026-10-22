# AMOLED H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_zlib_tar/loader/amoled:package
```

内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 `amoled-h2loader-esp32s3.update.tar` 和 release metadata；ESP-IDF app descriptor 和 package manifest 都使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=amoled`、`target=esp32s3`、`active_role=loader`、`transport=iostreamikcp` 的设备。使用 scan 返回的 port 执行 `status`，确认没有有效 Stage 后通过 `send` 发布 `update.tar.zlib` 并执行 `reboot upgrade`。

更新流程、完成条件和断电复查见 [Loader 更新](../../update/loader)。已经安装 H2Loader 的正常路径不直接烧录；只有 H2Loader 无法通信、重新确认 board/target identity 且已获得 destructive recovery 授权时，才按恢复流程使用匹配的 `.recovery.h2fb`。
