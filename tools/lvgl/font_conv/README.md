# LVGL font converter

`lvgl_font` wraps the pinned official `lv_font_conv` package as a public Bazel rule. Consumers own the source font, UTF-8 symbol sources, fixed pixel size, output symbol, and generated target wiring; GizOS owns the converter version, Node dependency graph, hermetic execution, validation, and failure behavior.

The rule reads declared symbol sources as strict UTF-8 and includes every unique non-control character. `ranges` can add stable sets such as printable ASCII. The action invokes the hermetic Node executable and official package entry module directly, so arbitrary Unicode, CMD metacharacters, and remapped range syntax never cross an npm batch launcher on Windows. The official converter rasterizes one requested size and emits an uncompressed LVGL C font, so the generated source does not require TinyTTF or `LV_USE_FONT_COMPRESSED` at runtime.

```starlark
load("@gizos//tools/lvgl/font_conv:font_conv.bzl", "lvgl_font")

lvgl_font(
    name = "status_font_16",
    font = "//assets:ui.ttf",
    font_name = "status_font_16",
    out = "status_font_16.c",
    ranges = ["0x20-0x7E"],
    size = 16,
    symbol_sources = [":ui_sources"],
)
```

The generated target is a normal C source and can be listed in `cc_library.srcs` or `firmware_native_component.native_srcs`. The converter and runner execute in the host configuration even when the generated source belongs to an embedded target.

```sh
bazel test //tools/lvgl/font_conv:all
bazel run //tools/lvgl/font_conv:lv_font_conv -- --version
make bazel-test-downstream-consumer
```
