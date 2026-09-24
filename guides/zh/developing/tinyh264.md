# TinyH264

`libs/pal/providers/tinyh264` 把 `@h2_vendor_tinyh264` 固定版本封装成 portable Video Decoder PAL provider。Public header 不暴露 upstream 类型；V1 支持 H.264 Baseline/Constrained Baseline Annex-B，输出 allocator-backed YUV420P 或 RGB565。

Upstream 的 `malloc`/`free` 通过 package-private bridge 路由到 session 的 `h2_pal_mem_api_t`。压缩 access unit 在 `submit_packet` 返回前被同步消费，decoded picture 会复制到 reusable PAL output storage，因此调用方不会借到 TinyH264 DPB reference picture。

Allocator 选择使用调用方持有的栈上 scope；默认通过 `_Thread_local` 隔离并发 decoder 线程的 allocator 指针，嵌套 scope 退出时恢复前一个 allocator。没有可用 TLS 的 target port 可定义 `H2_TINYH264_SCOPE_TASK`（返回当前 task identity）和 `H2_TINYH264_SCOPE_YIELD`（按毫秒 sleep/yield），切换到按 task identity 索引的 scope node registry；短时 atomic gate 仅在链接、移除和查找 node 时持有。该文件级 gate 使用 `H2_ATOMIC_DEFINE_STATIC` 拥有独立 static backing，无模块级 init/shutdown；默认 TLS port 不使用该 gate。当前 JieLi atomic provider 的 flag 操作仍是 `UNSUPPORTED` trap，相关 image 只要求编译，不能从静态 backing 推断其运行能力。

ESP-IDF image 通过 `native_component_src/esp-idf6.x/h2_tinyh264` 构建同一份 `libs/pal/providers/tinyh264` 与 `@h2_vendor_tinyh264` portable provider。ESP32-P4、ESP32-S3 和其他支持该能力的 ESP target 不使用 target-private decoder wrapper，统一暴露同一个 Video Decoder PAL contract。

```sh
bazel test //libs/pal/providers/tinyh264:all
```
