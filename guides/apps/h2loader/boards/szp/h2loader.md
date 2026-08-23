# SZP H2Loader <Badge type="warning" text="WIP" />

## 构建

```sh
bazel build --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/szp:package
```

内部 `bazel-bin/.../firmware/` 保存 raw image 与 recovery bundle，最终 `bazel-bin/.../package/` 保存 managed package 和 release metadata；ESP-IDF app descriptor 和 package manifest 都使用同一个 Bazel firmware version。Board defaults 固定启用 PSRAM XIP。

## Partition layout

待补充 H2Loader image 使用的 partition table 和各分区用途。

## sdkconfig

待补充 H2Loader image 使用的 `sdkconfig` defaults 和关键配置。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=szp`、`target=esp32s3`、`active_role=h2loader`、`transport=iostreamikcp` 的设备。使用 scan 返回的 port 执行 `status`，确认 idle 后通过 `send` stage `update.tar.zlib` 并执行 `upgrade`。

验收必须覆盖 trial、canonical 重连、最终 `active_version=<version>`、canonical running partition、`upgrade_phase=idle` 和 power-cycle 后复查。已经安装 H2Loader 的正常路径不直接烧录；只有 H2Loader 无法通信、重新确认 board/target identity 且已获得 destructive recovery 授权时，才按恢复流程消费匹配的 `.recovery.h2fb`。
