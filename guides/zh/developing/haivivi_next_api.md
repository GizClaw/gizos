# Haivivi Next API

`libs/haivivi_next_api` 是由 Haivivi Next OpenAPI contract 生成的
target-independent C library。它直接消费调用方注入的 HTTP、JSON 和 Memory
PAL，不拥有 PAL backend、网络配置、设备 Runtime 或产品流程。

## Source of truth

生产 contract 来自 `https://api.haivivi.cn/openapi-json`。`MODULE.bazel` 用 URL
和 SHA-256 固定输入；Bazel 只在 digest 匹配时把文档交给
`//tools/openapi_codegen:openapi_codegen`。生成的 C/header 是 Bazel action
output，不提交副本，也不允许手改。

当前 pin 的 SHA-256 是
`cd0dda64d35532233cc23f98a8242a746082f9cfbbee1aef7fb9fd5b601e3d53`，
对应 OpenAPI 3.0.0、157 个 path、172 个 operation 和 261 个 component schema。
`tests/test_contract.c` 固定 operation 数量，并编译检查上游重复
`operationId=legacyrestrictToken` 生成的两个独立 API。

稳定 target：

```text
//libs/haivivi_next_api:haivivi_next_api
```

该 target 的 public header 是 `h2_haivivi_next_api.h`。调用方配置 base URL、
bearer token、URL/request/response/string/array 上限及三个 PAL API。请求参数和
body 在同步调用期间借用；response 拥有解码后的字符串、数组、动态 JSON 和
响应头，必须由对应 `*_response_deinit()` 释放。

## Update and validation

服务 contract 变化时，先取得新 JSON 的 SHA-256，审查 OpenAPI semantic diff，
再同时更新 `MODULE.bazel` 的 digest、生成器兼容性测试和调用方。不得把浮动网页
内容直接带入 firmware build。

最低验证：

```sh
bazel test --config=macos_arm64 //tools/openapi_codegen:all
bazel build --config=macos_arm64 //libs/haivivi_next_api:haivivi_next_api
```

第二条命令会实际下载固定 contract、生成全部 endpoint 的 C，并以仓库 C11
warning-as-error 规则编译；仅运行 Go unit test 不能替代这项验证。
