# Portable cryptographic constructions

This package owns portable cryptographic constructions, signature representation and validation over caller-injected Crypto PAL. SM3 and SHA-256 remain independent algorithm packages. Raw random, digest, HMAC, cipher and elliptic-curve operations use the public PAL directly; this package neither selects a concrete provider nor duplicates those algorithms or their PAL entry points.

[h2_crypto.h](include/h2_crypto.h) defines complete SEC1 P-256 point validation, canonical DER signature encoding, AES-CBC/PKCS#7 padding, RFC 4226 HOTP and byte comparison without data-dependent early return. Alipay-specific repeating/reversed XOR masks, the noncryptographic multiply-by-31 hash and the legacy MD5/lowercase-hex AES key/IV derivation are private to [the Alipay package](../../clients/alipay/README.md). Protocol key material, identity choice, wire encoding and payer-code formatting stay with the protocol owner. Ciphertext authentication remains the caller's protocol responsibility.

SM2 identity binding uses the existing `libs/crypto/sm3` implementation for `SM3(Z || message)` and PAL for key derivation, signing. The provider receives an already composed digest and supplies fresh production nonces. No second SM3 implementation is compiled into the SM2 provider. Generic DER encoding accepts fixed-width ECDSA signatures with scalar sizes up to 32 bytes; protocol owners choose their actual wire representation.

The ISO/IEC 9797-1 algorithm 3 retail MAC uses padding method 2, a zero initial chaining value and the injected DES/EDE2 operations. It returns all eight MAC bytes without allocation; protocol-specific truncation and packet layout remain caller-owned.

Run `:crypto_test` for RFC 4226 vectors, PKCS#7 length/padding boundaries, output guard bytes, allocator/provider failures, P-256 coordinate validation, secp192k1 signature representation and SM2 identity/message binding through the test-only upstream verifier. Fixed nonce vectors are verified independently, and ordinary production signing is also exercised with its real RNG. The test selects wolfCrypt through a test-only provider facade; production callers inject their own provider. Alipay's `:crypto_compat_test` owns mask, compatibility-hash and derived-keyset regressions.

Production exposes SM2 signing only. Raw-digest verification lives in the existing `wolfcrypt_test_support` facade for independent signature/vector and mutation checks; it is absent from the public PAL and production constructions package.
