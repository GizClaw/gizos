# LVGL YAML i18n converter

This repository host tool validates locale-rooted YAML catalogs and generates a
deterministic C/H static catalog for LVGL `lv_translation_add_static`.

The converter has no product defaults. Callers must provide the input and output
directories, ordered locales, and the C symbol prefix:

```sh
bazel run //tools/lvgl/conv_i18n_yaml:conv_i18n_yaml -- \
  --input path/to/translations \
  --output path/to/generated \
  --locale zh-CN \
  --locale en-US \
  --symbol-prefix product_i18n
```

Pass `--check` to compare generated content with committed output without
rewriting it. Every `<locale>.yml` file must contain exactly one matching locale
root. Catalogs must have identical non-empty keys and matching `printf`
conversion specifiers.
