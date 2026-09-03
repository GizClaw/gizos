# QR Code

`libs/qrcode` 提供 QR Code Model 2 的编码与 RGB565 栅格化。它把 vendored
`qrcodegen` 包装成 GizOS contract：所有缓冲区由调用方提供，library 自身不分配内存、
不依赖平台，也不引用 Display PAL。

## Ownership

```text
libs/qrcode/
├── include/h2_qrcode.h                   # 编码、布局与 band 栅格化 contract
├── src/h2_qrcode.c                       # qrcodegen 包装与 RGB565 栅格化
└── tests/                                # Bazel 驱动的跨平台测试
```

上游 `nayuki/QR-Code-generator` 由 `third_party/qrcodegen.BUILD.bazel` overlay 装配，
consumer 通过 `//tools/bazel:vendor_third_party_qrcodegen` 取得。
`h2_qrcode.h` 不 include 上游 header，调用方的 include path 上只有 GizOS contract。

## 缓冲区

`h2_qrcode_encode_text()` 需要两块互不重叠、长度至少
`H2_QRCODE_BUFFER_LEN_FOR_VERSION(max_version)` 的 caller-provided storage：

- `modules` 保存编码结果，`h2_qrcode_t` 借用它，调用方必须保证其生命周期覆盖后续
  使用；
- `scratch` 只在调用期间有效，返回后内容无意义。

`H2_QRCODE_BUFFER_LEN_FOR_VERSION()` 是编译期常量表达式，可以直接作为数组长度。
Version 10 对应 468 字节，足以容纳典型的配网 URL；固件不需要为 version 40 预留
3918 字节。

## 布局与栅格化

`h2_qrcode_layout_center()` 在给定像素表面内选择能整除的最大 module 尺寸，并居中
符号；表面装不下符号加 quiet zone 时返回 `H2_PAL_ERR_NO_SPACE`，不会输出被裁切的
布局。Quiet zone 至少为规范要求的 `H2_QRCODE_QUIET_MODULES_MIN`。

`h2_qrcode_render_rgb565_band()` 每次只栅格化表面的一条整宽横条，调用方据此按条
推送到 Display PAL，避免整屏 framebuffer。符号内部使用 `dark_color` 与
`light_color`，quiet zone 使用 `light_color`，quiet zone 之外使用
`background_color`。

## 验证

```sh
bazel test //libs/qrcode:all
```

`qrcode` Example 消费该 library，Desktop 与 AMOLED 入口见
[Examples](/apps/example) 与 [AMOLED QR Code](/apps/h2loader/boards/amoled/qrcode)。
