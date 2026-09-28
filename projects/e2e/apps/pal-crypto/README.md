# PAL Crypto E2E

独立 portable App 验证现有 Crypto PAL 的全部 15 个操作，首版 registry 有 22 个必选 case。App 只借用 Runtime 的真实 Crypto provider，不导入 wolfCrypt/mbedTLS/OpenSSL 私有 API，不自行替换随机源。目标平台与前两项一致：macOS、WASM、iOS、Android、DevKit、BK7258；每个平台需要独立运行证据，不能用编译或另一平台结果代替。

测试包括标准 known-answer vectors、生成密钥后独立推导/双向共享秘密、三种 AEAD 的认证与篡改拒绝、原地/重叠缓冲区、容量及整数边界、无效密钥和签名、固定 P-256 签名校验以及重复调用。输出只记录 case ID、状态、错误码，不输出生成的私钥或随机 bytes。`qualified` 必须全部 case PASS；缺失 API/任何 callback 的 provider 不能执行或获得资格。随机数据差异检查仅发现常见接线故障，不构成随机质量、密码认证或侧信道安全证明。

标准 fixture 来源：[RFC 7748 §6.1](https://www.rfc-editor.org/rfc/rfc7748#section-6.1)、[RFC 5869 Appendix A.1](https://www.rfc-editor.org/rfc/rfc5869#appendix-A.1)、[RFC 8439 §2.8.2](https://www.rfc-editor.org/rfc/rfc8439#section-2.8.2)、AES-CTR SP800-38A、AES-GCM empty-message vectors、RFC 1321/RFC 2202。字节值与仓库已有 provider 单测 fixture 一致，但独立 App 不链接或调用那些单测；P-256 固定签名 fixture 也保持原来的固定消息和 raw r||s 编码。MD5/HMAC-SHA1 只按已有协议兼容接口验收。

```sh
bazel test //projects/e2e/apps/pal-crypto/app:interface_coverage_test \
  //projects/e2e/apps/pal-crypto/app:rejection_test \
  //projects/e2e/targets/cc_binary/pal-crypto:desktop_test \
  //projects/e2e/targets/pkg_tar/pal-crypto:browser_test
H2_IOS_SIMULATOR_UDID=<booted-uuid> make bazel-test-ios_pal_crypto_simulator_test
H2_ANDROID_SERIAL=emulator-5580 make bazel-test-android_pal_crypto_simulator_test
```

Desktop 注入真实 Darwin/Linux 系统 entropy。Provider 只在 launcher 初始化和销毁，不在 case 之间替换；结果 JSON 由测试输出目录保存。每个平台 launcher 必须提供独立进程/设备 watchdog，避免 provider 阻塞导致无限运行。

WASM 在真实 Chromium 的 Worker 中执行；iOS、Android 从实际打包的 Swift Package/AAR 消费平台 provider，App Host 默认绑定使用系统熵的 wolfCrypt 实现。移动端每次安装后删除旧 report 并启动 App，由宿主核对实际版本、完整 registry、退出和 teardown；模拟器结果不能代表手机实机验收。

DevKit、BK7258 使用板级 Runtime 与 H2Loader managed serial 安装。设备每次 boot 只运行一次，随后 replay 不可变 case ledger；重连获取 replay 不算重新执行。安装前后核对 UID、镜像和 package 摘要、Loader/P1、Stage、App/P2 与 coredump。构建、接线和安装约束见对应 `targets/h2loader_tar_zlib/pal-crypto` README。

测试发现并修复了 ESP 零长度随机请求触发 SDK assert、ESP/BK 对 X25519 低阶公钥的错误归类、BK PSA 配置未传播造成的 operation object 越界，以及 BK TrustEngine 对 Curve25519/压缩 P-256 公钥的能力缺口。BK AP 选择 SDK 官方软件 Crypto 分支，并通过标准 `mbedtls_hardware_poll` 接入 TRNG，避免该 SDK 分支默认 `rand()` stub 进入 PSA 内部 DRBG；不复制第三方密码源码或自行实现曲线运算。

## 六平台实测

2026-09-28 的结构化记录汇总于 [qualification.json](qualification.json)，保存每个平台完整 ledger 和 evidence SHA-256。下列结果对应记录中的实际构建产物；后续源码变化需要重新验证，不能将这些历史记录视为新版本自动通过。

| 平台 | 实际环境 / Provider | 结果 |
| --- | --- | --- |
| macOS | 原生进程、Darwin entropy、wolfSSL Crypto | 22 PASS，0 FAIL/BLOCKED |
| WASM | 真实 Chromium、Worker、Web Crypto 随机源与 wolfCrypt | 22 PASS，0 FAIL/BLOCKED |
| iOS | iOS 26.5 Simulator、实际 Swift Package、SecRandomCopyBytes | 22 PASS，0 FAIL/BLOCKED |
| Android | API 36 arm64 Emulator、实际 AAR、系统 urandom | 22 PASS，0 FAIL/BLOCKED |
| DevKit | ESP32-S3 实机、PSA/mbedTLS，`pal-crypto-esp-20260928-r5` | 22 PASS，0 FAIL/BLOCKED |
| BK7258 | AP 实机、SDK 软件 PSA/mbedTLS，`pal-crypto-bk-20260928-r4` | 22 PASS，0 FAIL/BLOCKED |

BK 软件 EC 计算耗时明显增加，本次 X25519 known-answer case 约 23 秒；功能资格不代表原 TrustEngine 的性能。安装 monitor 重连后获取同一 boot 的完整结果，未重启测试。最终 App 已确认、Stage 为空、原 Loader 保留；BK 原有 32-byte coredump 保持逐字节相同，DevKit 最终 coredump 为空。开发过程中较早镜像的失败记录不计为通过。
