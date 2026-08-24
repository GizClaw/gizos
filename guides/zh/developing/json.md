# JSON

GizOS 使用 `h2_pal_json_api_t` 作为 portable JSON contract。Portable 调用方只接触
`h2_pal_json_document_t`、`h2_pal_json_value_t` opaque handle 和 PAL wrapper，不能
include yyjson header，也不能根据 target macro 选择 backend。

## Provider

仓库提供一套 portable provider：

| Provider | 目录 | 上游 | 适用边界 |
| --- | --- | --- | --- |
| yyjson | `libs/pal/providers/yyjson` | `@h2_vendor_yyjson` 0.12.0 | 面向 firmware 的独立实例 provider；每个实例持有独立 allocator 和 lifecycle |

Provider 创建时复制 `h2_pal_mem_api_t`，但不取得 Memory PAL backend 的 ownership。
调用方必须先释放该 provider 创建的 serialized buffer 和 document，再销毁 provider；
存在 live object 时销毁返回 `H2_PAL_ERR_INVALID_STATE`。

Desktop target owner 的 registry、Serial Loader cache 和 H106 normalized layout parser
直接使用 pinned yyjson，因为这些 target-owned 格式不进入 portable App contract，且 cache
上限大于 PAL document 上限。Direct vendor header 不能从这些 target package 泄漏到 portable
library、PAL contract 或 App public API。

Provider 选择属于 composition owner，不属于 PAL、Runtime 或 portable App。本次集成只
为 ESP-IDF 6.x 和 BK7258 AP 提供 yyjson SDK build adapter，并让指定的
crash-before-confirm image 编译链接它；这不等于把 JSON capability 加入 Runtime。

## 数据与 ownership

Document 拥有其完整 value tree。解析、创建、查询、修改和 compact serialize 都通过
同一个 provider API 完成：

```text
Memory PAL
  -> yyjson provider
      -> document
          -> opaque value handles
      -> serialized buffer
```

- 输入 JSON bytes 只在同步 parse 调用期间借用。
- String getter 返回 decoded UTF-8 borrowed view，在 document 销毁或其所属 subtree 被替换前有效。
- 新 value 最初由 document 拥有但未 attach；成功设置 root、array append 或 object set
  后成为 tree 的一部分。
- 同一个 value 不能重复 attach，不能跨 document attach，也不能形成 cycle。
- Object replacement 使被替换 value 及其 subtree 的旧 handle 失效。
- Serialize 返回 provider-owned compact JSON buffer；只能交给同一 provider 的
  `h2_pal_json_buffer_release()`。
- Document destroy 和 buffer release 都消费 caller handle，并在成功后清零。

Object key 和 string value 使用 pointer + length；`NULL + 0` 表示合法空字符串，
`NULL + nonzero` 是 invalid argument。Object 保持 JSON object 语义，不承诺 key 顺序。
`h2_pal_json_object_size()` 返回当前 member 数量；`object_entry()` 按 provider-specific
顺序返回 borrowed key/value，越界返回 `H2_PAL_ERR_NOT_FOUND` 并清零 output。任何
object mutation 都会使调用方保存的 iteration index 失去继续遍历的语义，调用方必须
重新从 index 0 开始；portable code 不能把该顺序写入稳定输出或业务判断。

## 严格解析

yyjson provider 对外提供 RFC 8259-compatible contract：

- 输入必须是一个完整 JSON value，前后不能有第二个 value、comment 或 trailing comma。
- UTF-8、escape 和 surrogate pair 必须有效；BOM 和 decoded `U+0000` 被拒绝。
- Duplicate object key 被拒绝。
- Number 必须能表示为 finite `double`；NaN、Infinity 和 overflow 被拒绝。
- Parse format error 返回 `H2_PAL_ERR_FORMAT`，资源限制返回
  `H2_PAL_ERR_NO_SPACE`，allocator failure 返回 `H2_PAL_ERR_NO_MEMORY`。

默认限制为 64 KiB document bytes、32 层 nesting 和 4096 个 value。调用方可以传入
更小的非零限制，但不能超过默认上限。Byte limit 约束 parse input 和 serialized output；
depth/value limit 同时约束 parse 和 construction，超限 attach 不得部分修改 tree。

## 错误与 unsupported

所有 public operation 都有 `h2_pal_json_*()` wrapper。Wrapper 先校验 caller 参数并清零
output：NULL API 或 canonical unsupported API 返回 `H2_PAL_ERR_UNSUPPORTED`；非 NULL
但缺少 vtable/operation 的 API 返回 `H2_PAL_ERR_INVALID_ARG`。Type mismatch、stale handle、
重复 attach 或存在 live object 的 provider destroy 返回
`H2_PAL_ERR_INVALID_STATE`；missing key/index 返回 `H2_PAL_ERR_NOT_FOUND`。

不支持 JSON 的 composition owner 使用 `h2_pal_unsupported_json_api()`。不能用 NULL vtable、
target-specific stub 或直接链接 vendor API 代替 canonical unsupported object。

## 构建与验证

`libs/pal:json_conformance` 被 yyjson provider 的 native test 复用，验证 strict parse、query、
round-trip、mutation/ownership、cross-document、cycle、stale handle、resource limit、
逐点 allocator failure 和完整清理。测试同时报告同一基准 document 的 allocator peak，
用于比较 provider，而不是声明设备运行时的整机内存占用。

```sh
bazel test //libs/pal:all
bazel test //libs/pal/providers/yyjson:all
```

Firmware compile validation 使用：

- ESP32-S3 DevKit crash-before-confirm。
- ESP32-P4 Waveshare crash-before-confirm。
- BK7258 V3 202405 crash-before-confirm AP；CP 不注册或链接 `h2_yyjson`。

ESP32-C5 当前没有 maintained launcher，记录为 `SKIP`。这三项是 build/link
carrier，不执行 JSON runtime flow，也不替代真实设备验收。
