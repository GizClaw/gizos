# Fruit Slice Chinese font

Fruit Slice uses the shared [Droid Sans Fallback Regular source](../../../../libs/ui/fonts/droid_sans_fallback/README.md), version 2.55b, through `//libs/ui/fonts/droid_sans_fallback:DroidSansFallback.ttf`. The public resource retains the unmodified Android 4.4 source, its exact hash and upstream provenance. It covers every Chinese character and full-width punctuation mark in the Game UI.

The font carries Google's 2006 digitized data copyright and is distributed under the Apache License 2.0. The original AOSP notice has SHA-256 `38751245389e1e23f73e6f5384b5cbe7fa972cc4410c5adc9c04b082a0b9561a`. The full notice, font attribution and description of the bitmap conversion travel with the linked font asset in `license_notice`. [Bundled notice](NOTICE.txt) · [AOSP notice source](https://android.googlesource.com/platform/frameworks/base/+/1aed5ea2dcf7cb0bf35635f378b2326461ba0df0/data/fonts/NOTICE)

The App build generates uncompressed 12, 15, 20 and 26 px strikes from its UTF-8 translation table using the repository's pinned `lv_font_conv`. Compact and body text use 4-bit coverage. Headings use 2-bit coverage. The renderer retains the 5×7 Latin face and draws Chinese at the native strike size. A single common baseline per strike keeps mixed text within existing cell and line bounds. Only the generated bitmap tables and attribution are linked. The original TTF is a build input. [Build rules](../BUILD.bazel) · [Translation table](../src/h2_fruit_slice_text.c)

```sh
bazel build //projects/fruit_slice/app:fonts
bazel test //projects/fruit_slice/app:text_test //projects/fruit_slice/app:render_test //projects/fruit_slice/app:app_test
```

`text_test` verifies coverage of the complete translation table in all four strikes, bounds and clipped bitmap coverage. `app_test` can capture RGB565 frames from the public App entry with simulated Display and Touch PALs when `TEST_UNDECLARED_OUTPUTS_DIR` is set.
