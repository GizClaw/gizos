# Fruit Slice Chinese font

The UI uses the existing [`NotoSansSC-Bold.ttf`](../../../../libs/ui/fonts/noto_sans_sc/NotoSansSC-Bold.ttf), SHA-256 `b905aa13f6c6986c7c440ca32c61e034a629967e41e9e46e2208c8ead8a2ed72`. Its embedded family is **Noto Sans SC**, version `2.004-H2`; its embedded copyright is `(c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.` This is the repository's shared Chinese font input. The current tree has no `firmwares/` directory or Droid font asset; this subset is not Droid Sans Fallback.

Noto CJK Sans is distributed under the [SIL Open Font License 1.1](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE). The upstream license file has Git blob SHA `d952d62c065f3f35fb83a173496e90b21525aef3`. The full notice is retained in [OFL.txt](OFL.txt) and in the linked font asset's `license_notice` metadata, so binary consumers retain the attribution when distributing the bitmap subset. The subset's primary font name is `h2_fruit_slice_font_zh`; the reserved name Source is not used for the derivative. Application code remains under the repository license; font data remains under OFL.

[`../BUILD.bazel`](../BUILD.bazel) uses the repository's pinned `lv_font_conv` chain to generate one uncompressed 14px, 2-bit bitmap subset from the UTF-8 translation table [`h2_fruit_slice_text.c`](../src/h2_fruit_slice_text.c). Generated C stays in Bazel outputs. Both Latin and Chinese source symbols are accepted by the converter; the renderer retains its original 5x7 Latin path and reads only the Chinese glyphs from the generated table. No complete TTF is linked, no LVGL/TinyTTF runtime is needed, and text does not allocate during play. The 15px ink envelope includes CJK overshoots; integer area sampling preserves thin strokes when reducing it into a 12px compact label. Drawing and measurement share the UTF-8 walk and character advances.

Reproduce the subset and verify glyph coverage, malformed UTF-8 handling, draw bounds and language touch input:

```sh
bazel test --config=macos_arm64 //projects/fruit_slice/app:text_test //projects/fruit_slice/app:render_test
bazel build --config=macos_arm64 //projects/fruit_slice/app:zh_font
```

Use the host's normal Bazel configuration on another platform. `text_test` walks every translated Unicode code point against the generated font and checks the complete ink envelope; missing glyphs or new overshoots fail the test. Keep new on-screen Chinese strings in the translation table so regeneration and coverage remain automatic. `render_test` emits **render previews**, including 160x160, 240x240, AC791N's 480x320 and mobile 480x800 geometry. These deterministic composites are not actual device screenshots or proof of device frame rate.
