# PAL Crypto E2E

独立 portable App 验证现有 Crypto PAL 的全部 15 个操作，首版 registry 有 22 个必选 case。App 只借用 Runtime 的真实 Crypto provider，不导入 wolfCrypt/mbedTLS/OpenSSL 私有 API，不自行替换随机源。目标平台与前两项一致：macOS、WASM、iOS、Android、DevKit、BK7258；每个平台需要独立运行证据，不能用编译或另一平台结果代替。

测试包括标准 known-answer vectors、生成密钥后独立推导/双向共享秘密、三种 AEAD 的认证与篡改拒绝、原地/重叠缓冲区、容量及整数边界、无效密钥和签名、固定 P-256 签名校验以及重复调用。输出只记录 case ID、状态、错误码，不输出生成的私钥或随机 bytes。`qualified` 必须全部 case PASS；缺失 API/任何 callback 的 provider 不能执行或获得资格。随机数据差异检查仅发现常见接线故障，不构成随机质量、密码认证或侧信道安全证明。

标准 fixture 来源：[RFC 7748 §6.1](https://www.rfc-editor.org/rfc/rfc7748#section-6.1)、[RFC 5869 Appendix A.1](https://www.rfc-editor.org/rfc/rfc5869#appendix-A.1)、[RFC 8439 §2.8.2](https://www.rfc-editor.org/rfc/rfc8439#section-2.8.2)、AES-CTR SP800-38A、AES-GCM empty-message vectors、RFC 1321/RFC 2202。字节值与仓库已有 provider 单测 fixture 一致，但独立 App 不链接或调用那些单测；P-256 固定签名 fixture 也保持原来的固定消息和 raw r||s 编码。MD5/HMAC-SHA1 只按已有协议兼容接口验收。

```sh
bazel test //projects/e2e/apps/pal-crypto/app:interface_coverage_test \
  //projects/e2e/apps/pal-crypto/app:rejection_test \
  //projects/e2e/targets/cc_binary/pal-crypto:desktop_test
```

Desktop 注入真实 Darwin/Linux 系统 entropy。Provider 只在 launcher 初始化和销毁，不在 case 之间替换；结果 JSON 由测试输出目录保存。每个平台 launcher 必须提供独立进程/设备 watchdog，避免 provider 阻塞导致无限运行。
