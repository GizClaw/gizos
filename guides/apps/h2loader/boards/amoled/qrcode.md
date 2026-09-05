# AMOLED QR Code

`qrcode` image 把固定文本编码成 QR Code 符号，并居中绘制到 368×448 的 AMOLED 屏幕上。
Portable App 见 [Examples](/apps/example)，编码与栅格化见
[QR Code](/zh/developing/qrcode)。

## 构建

```sh
bazel build \
  --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/example/targets/h2loader_tar_zlib/qrcode/amoled:package
```

输出位于 `bazel-bin/projects/example/targets/h2loader_tar_zlib/qrcode/amoled/package/amoled-qrcode-esp32s3.update.tar.zlib`。安装、确认和恢复必须遵循 [H2Loader CLI 使用流程](/zh/using/h2loader/cli)。

## 预期表现

- 启动日志包含 `H2_QRCODE_EXAMPLE_READY text=https://github.com/GizClaw/gizos`。
- 屏幕以 90% 亮度显示黑白 QR Code，四周是白色 quiet zone，符号居中且未被裁切。
- 手机相机扫描该符号得到 `https://github.com/GizClaw/gizos`。
- 画面保持静止；App 在 present 成功后确认 H2Loader app image。
