# Fruit Slice Chinese font

The UI uses the existing [`NotoSansSC-Bold.ttf`](../../../../libs/ui/fonts/noto_sans_sc/NotoSansSC-Bold.ttf), SHA-256 `b905aa13f6c6986c7c440ca32c61e034a629967e41e9e46e2208c8ead8a2ed72`. Its embedded family is **Noto Sans SC**, version `2.004-H2`; its embedded copyright is `(c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.` This is the repository's shared Chinese font input. The current tree has no `firmwares/` directory or Droid font asset; this subset is not Droid Sans Fallback.

Noto CJK Sans is distributed under the [SIL Open Font License 1.1](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE). The upstream license file has Git blob SHA `d952d62c065f3f35fb83a173496e90b21525aef3`. The full notice is retained in [OFL.txt](OFL.txt) and in the linked font asset's `license_notice` metadata, so binary consumers retain the attribution when distributing the bitmap subset. The bitmap subsets use the `h2_fruit_slice_font_zh_*` names. Application code remains under the repository license and font data remains under OFL.

[`../BUILD.bazel`](../BUILD.bazel) generates uncompressed 12, 15, 20 and 26 px strikes with the repository's pinned `lv_font_conv` from the UTF-8 translation table [`h2_fruit_slice_text.c`](../src/h2_fruit_slice_text.c). The compact and body strikes use 4-bit coverage to retain finer edges in small glyphs. Headings use 2-bit coverage to limit flash use. The app draws Chinese at the selected strike's native size and retains its original 5×7 Latin path. At 480×320, description lines request the 15 px strike and fall back to compact when space is tight. Drawing and measurement share cell widths and line heights that include the glyph overshoots. Generated C stays in Bazel outputs. Text uses the existing RGB565 canvas and needs no heap allocation or runtime font engine.

Reproduce the subset and verify glyph coverage, malformed UTF-8 handling, draw bounds and language touch input:

```sh
bazel test --config=macos_arm64 //projects/fruit_slice/app:text_test //projects/fruit_slice/app:render_test
bazel build --config=macos_arm64 //projects/fruit_slice/app:fonts
```

Use the host's normal Bazel configuration on another platform. `text_test` checks each translated Unicode code point at every size, the font metrics, and clipped coverage against the native bitmap. Keep new on-screen Chinese strings in the translation table so regeneration and coverage remain automatic. `render_test` emits deterministic render previews at 160×160, 240×240, AC791N's 480×320 and mobile 480×800. `app_test` captures RGB565 frames from the public App entry with simulated Display and Touch PALs when `TEST_UNDECLARED_OUTPUTS_DIR` is set.
