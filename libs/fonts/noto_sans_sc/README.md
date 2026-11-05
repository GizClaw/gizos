# Noto Sans SC font resource

`NotoSansSC-Bold.ttf` is the complete Chinese UI font input shared by the
manufacturing UI, LVGL font/subset tools and external-consumer build tests.
Consumers reference `//libs/fonts/noto_sans_sc:NotoSansSC-Bold.ttf` and generate
the glyph subset required by their own UI.

The font bytes and embedded metadata are retained unchanged. The embedded family
is Noto Sans SC, version `2.004-H2`; its copyright metadata identifies
Adobe, 2014–2021, with the reserved font name Source.

Run the subset consumer regression without an SDK or device:

```bash
bazel test --config=macos_arm64 //tools/lvgl/ttf_subset:fixture_font_test
```
