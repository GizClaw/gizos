# CoreHTTP

`libs/pal/providers/corehttp` 把固定版本的 AWS coreHTTP 和其内固定的 llhttp 集成为
`h2_pal_http_api_t` provider。它是 target-independent library；Desktop、ESP-IDF
和 BK7258 Runtime composition root 创建相同 provider，不再选择平台 HTTP client。
ESP-IDF 与 BK7258 使用所选 Bazel embedded toolchain 编译这个 semantic target；
`h2_pal_core` 只导入其 archive closure，不再从 native SDK source list 重编 wrapper、
coreHTTP 或 llhttp。

## API Reference

[API Reference](/references/corehttp)

`h2_corehttp_create()` 借用 Memory、Net、Time PAL，可选借用 Log PAL，并复制
provider config 和显式 root CA。每次同步 request 拥有独立的 parser、socket、buffer、
deadline、retry 和 redirect 状态；销毁 provider 前必须等待全部 request 返回。

Board Runtime composition root 拥有 provider lifecycle。Runtime 初始化失败或正常
调用 `h2_runtime_deinit()` 后，caller 必须继续调用对应的 board Runtime deinit；ESP
和 BK board-owned entry task 在 entry 返回后统一执行该调用，因此 entry 必须先释放
Runtime。该 teardown 先销毁 coreHTTP 并清空 board 持有的 HTTP/config view，再释放
filesystem。更外层的 platform lifecycle 必须在此之后才能释放 Net 和 allocator 等
borrowed dependency。重复 teardown 是成功 no-op，teardown 后重新取得 Runtime
config 会创建新的 provider。

## Transport 与 TLS 边界

Provider 使用 coreHTTP public API 生成请求，用固定 llhttp 增量解析响应，并仅通过
Net PAL 完成 source-interface validation/bind、DNS、TCP、TLS 和 close。它不 include
或链接 curl、wolfSSL、Mbed TLS、`esp_http_client` 或 BK `webclient`。

`interface_name` 非空时，Provider 必须先通过 `get_host_addr` 验证并取得 source
address，再启动 DNS resolver；无效接口不能产生 DNS activity 或消耗 DNS deadline。

DNS 使用 Net PAL 的 asynchronous resolver。Provider 用与 TCP、TLS、send 和 receive
相同的 request 总 deadline 分段 poll lookup，并在每个 poll 边界检查 cooperative
cancellation；任何终态、timeout 或取消都会 close resolver。平台同步 resolver 只能在
有界的 backend-owned worker 中执行，不能让 portable request 直接阻塞在
`getaddrinfo`；request close 后仍未返回的平台 lookup 完成时自行回收。

HTTPS 调用现有 `h2_pal_net_vtable_t.tls_wrap`，传入 URL host、`http/1.1` ALPN、
验证模式和可选 root CA。Desktop 与 Windows 的 Net backend 使用静态 wolfSSL；ESP/BK 的 Net
backend 使用平台 Mbed TLS。BK 没有 production system CA bundle，`REQUIRED` 且没有
显式 root CA 时必须 fail closed；不能退化为不验证。

Provider 不提供连接池、HTTP/2、proxy、cookie、压缩或平台 credential provisioning。
每次 attempt 独占 connection，并在成功、失败或取消时释放 transport。

固定 llhttp source 中未被 production parser 调用的 `llhttp__debug()` 由私有 llhttp
Bazel target 的 forced-include config 保持为 no-op，不能向 `stderr` 输出，也不能
把 newlib standard-stream reentrancy 符号带入 firmware archive。这个配置不修改
verified vendor archive 或增加 textual patch；CoreHTTP 自身需要诊断时只使用 caller 注入的
Log PAL。

## 构建与测试

```sh
bazel test //libs/pal/providers/corehttp:all
```

Fake-Net tests 验证 request generation、增量 response parsing、body ownership、
DNS deadline、取消、retry、redirect 和 TLS 参数，并验证每条 resolver path 都 close。
共同 host PAL E2E 在 Linux、macOS 和 Windows 使用仓库内证书与 loopback
HTTP/HTTPS fixture 验证真实 `coreHTTP -> Net PAL -> wolfSSL` 路径。Firmware build
只能证明平台 adapter 可编译，不能替代真实设备上的 HTTPS、取消或 timeout 验证。
