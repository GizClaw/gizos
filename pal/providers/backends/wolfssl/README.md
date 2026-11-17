# wolfSSL and wolfCrypt PAL backend

The public [crypto configuration](include/h2_wolfcrypt_crypto.h) injects entropy and exposes stable Crypto PAL operations without leaking wolfCrypt types. The provider owns key import/export, hashing, ciphers, elliptic-curve operations and upstream cleanup; portable clients select it through their assembled PAL capability.

SHA-1 remains enabled for the existing HMAC-SHA1 operation used by HOTP and the legacy secp192k1/SHA-1 transit signer. MD5 remains enabled for the existing Alipay compatibility key derivation and message checks.

The wolfCrypt-only build uses `WOLFSSL_SP_MATH_ALL` with the regular 32-bit optimized P-256 path and generic arithmetic for legacy secp192k1 and SM2. The pinned upstream's `WOLFSSL_SP_SMALL` configuration produced invalid P-256 S values for the controlled-nonce regression in `//libs/crypto/constructions:crypto_test`; the regular path passes the fixed signature bytes, verification and original-SDK differential cases. Preserve that check when changing math configuration.

SM2 imports the immutable wolfSM source declared in `MODULE.bazel` into wolfSSL's placeholder files. The package-local compatibility patch omits optional ASN.1 and internal digest composition; the public crypto library owns SM3 identity binding using its existing implementation. Public PAL types contain only raw scalar, point and signature bytes.

ESP32-S3 assembly can select the same crypto-only backend and inject Runtime Crypto PAL entropy. The API getter returns canonical unsupported operations before initialization, so callers obtain the usable API after successful initialization. Upstream selects `CURVE25519_SMALL` on Xtensa; its low-memory field source is declared explicitly and RNG blinding setters are called only when that separate capability is enabled. This flag is distinct from the disabled P-256 `WOLFSSL_SP_SMALL` regression path.

The explicit `:wolfcrypt_test_support` facade is `testonly`. P-256 and secp192k1 nonce-controlled signing uses the real upstream signer with `WOLFSSL_ECDSA_SET_K`. SM2 uses a test-only compilation of the same pinned signer with its ephemeral-key generation call renamed to a synthetic fixture; production key generation is unchanged. Every controlled signature is verified by the actual upstream verifier. SM2 verification is private to this test-only facade; production exposes the signing operations used by native transit. These fixtures are not production PAL operations. Run `:crypto_test` and `//libs/crypto/constructions:crypto_test` for the provider and composed operations.
