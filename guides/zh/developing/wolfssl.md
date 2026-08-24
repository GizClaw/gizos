# WolfSSL

`libs/pal/providers/wolfssl` 是 `@h2_vendor_wolfssl` 的唯一 stable integration root，并从
同一份固定 upstream inventory 构建两个互斥 variant：

`third_party/wolfssl.BUILD.bazel` 拥有固定 source/header groups；`libs/pal/providers/wolfssl` 选择 user settings、compile definitions 和最终 variant。Overlay 不从 first-party package 反向加载 source inventory。

- `//libs/pal/providers/wolfssl:wolfcrypt_upstream` 与 `:wolfcrypt` 是受限固件使用的裁剪
  Crypto PAL provider，保留 `h2_wolfcrypt_crypto_*` lifecycle。
- `//libs/pal/providers/wolfssl:wolfssl_upstream` 与 `:wolfssl` 是 Desktop 等完整 target 使用的
  stream TLS、Crypto PAL 与 DTLS PAL provider，公开 `h2_wolfssl_*` lifecycle。

两个 target 共享 Crypto adapter，但完整 target 不依赖或链接裁剪 upstream。
Consumer 不能 include WolfSSL header、选择 upstream source 或定义 user setting。

## Lifecycle

`h2_wolfssl_init()` 要求完整 Memory PAL 与 secure entropy callback，并同步复制
API object。首次初始化安装 WolfSSL process-global allocator/RNG bridge，再初始化
共享 Crypto adapter 与 WolfSSL global state。使用同一份 Memory/entropy config 的重复 init
增加 process-global reference；不同 config 返回 `H2_PAL_ERR_INVALID_STATE`。
`h2_wolfssl_deinit()` 释放一个 reference；最后一个 live DTLS session 销毁前不能拆掉仍被
使用的 global state。

Desktop composition 只初始化完整 provider，并从 `h2_wolfssl_crypto_api()` 取得
Crypto PAL；DTLS consumer 直接使用 `h2_wolfssl_dtls_api()`，Desktop component 不再
拥有另一套 DTLS adapter 或 accessor。

Windows composition 同样只拥有一份完整 provider reference，使用 Windows Memory 与 BCrypt entropy callback。完整 upstream target 在 Windows 链接 `Crypt32.lib` 以读取系统 CA，并链接 `Ws2_32.lib` 满足 upstream DTLS/socket import；显式 PEM root 仍由调用方配置。Net、CoreHTTP 和 CoreMQTT consumer 必须先销毁，然后 deinit wolfSSL，最后销毁 Windows platform。

受限 target 直接组装裁剪 provider。BK3633 的 target component 只暴露 hardware TRNG callback；TapDoki BSP 先验证 TRNG 可用，再把该 callback 注入 `h2_wolfcrypt_crypto_init()`，并直接消费 `h2_wolfcrypt_crypto_api()`。Target 不能再创建一套转发 init/deinit/api 的 wrapper、ready 状态或第二个 Crypto PAL accessor。

## DTLS

每个 session 生成 self-signed ECDSA certificate，以完整 DER 的 SHA-256 raw bytes
作为 fingerprint。Remote fingerprint 必须在 handshake 完成前设置；相同值重复设置
idempotent，不同值被拒绝。握手只使用 DTLS 1.2 和
`SRTP_AES128_CM_SHA1_80`，keying material exporter label 固定为
`EXTRACTOR-dtls_srtp`，输出长度固定为
`H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE`（60 bytes）。

WolfSSL send callback 不直接借用 caller buffer：datagram 先复制进 session 的
`max_pending_output_bytes` queue，再同步尝试 caller send。队列溢出是 terminal
failure；pending output 存在时应用 write 不消费新 plaintext。Caller 驱动 flush、
datagram delivery 与 absolute retransmission deadline，provider 不创建后台执行单元。

## Validation

```sh
bazel test //libs/pal/providers/wolfssl:all
bazel test --config=macos_arm64 --nocache_test_results //libs/pal/providers/wolfssl:all
```

测试必须覆盖裁剪/完整 lifecycle、Crypto vectors、双端 DTLS handshake、fingerprint
failure、SRTP profile/exporter、plaintext records、output backpressure 与 hard deadline。
Linux、macOS 和 Windows 的共同 host PAL E2E 另外通过仓库内证书与 loopback peer 验证
本地 TLS；该结果不代表公共 trust store 或公网服务互操作性。
