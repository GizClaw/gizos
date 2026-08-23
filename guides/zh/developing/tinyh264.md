# TinyH264

`libs/pal/providers/tinyh264` 把 `third_party/tinyh264` 固定版本封装成 portable Video Decoder PAL provider。Public header 不暴露 upstream 类型；V1 支持 H.264 Baseline/Constrained Baseline Annex-B，输出 allocator-backed YUV420P 或 RGB565。

Upstream 的 `malloc`/`free` 通过 package-private bridge 路由到 session 的 `h2_pal_mem_api_t`。压缩 access unit 在 `submit_packet` 返回前被同步消费，decoded picture 会复制到 reusable PAL output storage，因此调用方不会借到 TinyH264 DPB reference picture。

ESP-IDF image 通过 `native_component_src/esp-idf6.x/h2_tinyh264` 构建同一份 `libs/pal/providers/tinyh264` 与 `third_party/tinyh264` portable provider。ESP32-P4、ESP32-S3 和其他支持该能力的 ESP target 不使用 target-private decoder wrapper，统一暴露同一个 Video Decoder PAL contract。

```sh
bazel test //libs/pal/providers/tinyh264:all
```
