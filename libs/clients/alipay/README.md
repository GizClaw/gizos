# Alipay client

This package owns the portable Alipay implementation and its public C/C++ compatibility ABI. Link `//libs/clients/alipay`; business declarations live in [h2_alipay_api.h](include/h2_alipay_api.h), injected configuration and lifecycle in [h2_alipay_context.h](include/h2_alipay_context.h), and explicit portable sessions in [h2_alipay_client.h](include/h2_alipay_client.h), [h2_alipay_scan.h](include/h2_alipay_scan.h) and [h2_alipay_tap.h](include/h2_alipay_tap.h). Builds require no external Alipay header tree, binary archive, environment path or native vendor adapter. Existing `alipay_*` entry signatures and `h2_alipay_independent_*` configuration symbols remain stable after the package rename.

The library implements synchronous business calls. Inject Memory, Time, HTTP, Pref, Sync and Crypto PAL contracts; join callers before replacing configuration or releasing state. The library selects no concrete provider, acquires no radio and owns no UI. NFC/BLE acquisition, user interaction, transmission and results presentation belong to [integration](../../../projects/testing/integration/alipay/README.md). Cryptographic algorithms and standard constructions use `libs/crypto` and injected Crypto PAL; provider selection stays in final assembly. Legacy wire-format masks, the wrapping multiply-by-31 hash and MD5/lowercase-hex AES key/IV derivation live in private `src/h2_alipay_crypto_compat.*` files and the package-private `:crypto_compat` target.

## Supported capabilities

`//libs/clients/alipay` provides the binding, account, payer/collection, NFC/BLE, text-based union-code and native transit implementations used by the H106 integration. It links neither an image decoder nor a script interpreter. QR generation and display use the existing encoder in the integration App.

A valid initialized image-parse call returns `99` with `H2_PAL_ERR_UNSUPPORTED` in its H2 diagnostic and preserves the caller's order. Script-backed transit generation/update returns `43` without downloading or executing a script. Native transit formats and text-based union-code parsing remain available.

## Binding and account

The network/ECDH profile uses child-watch product type 3 and PAYCODE | SQUARE, with optional NFCPAY and BLEPAY bits. Request signing, device registration, prepareBind, queryBindInfo, ECDH factor staging and finishBind share the configured identity and store. A completed binding requires validated TID, UID, shared key and seed plus a matching completion record. One query performs one bounded business step; polling is the caller's policy.

Completed records restore locally. A staged record after restart requires explicit verification before reuse. Failed staged commits send no finishBind; an acknowledged finish followed by a failed local commit retains enough state to retry locally. A lost server acknowledgement can cause another finish request; exactly-once server behavior is not claimed. An active or completed binding blocks a fresh binding-code request from replacing it.

Account getters, static privacy/settings URLs, remote binding status, balance bitmap, unbind/reset, provisioning, migration and synchronization preserve their public length and error semantics. Account text lengths exclude NUL; callers terminate presentation buffers themselves. Unbind/reset remove only owned account/context/cache records. Factory identity and unrelated namespaces remain caller-owned. Durable recovery requires durable Pref and separate restart evidence; a volatile provider establishes only session readback.

## Payment and NFC/BLE

Payer generation decrypts the bound seed through the crypto package and uses calibrated time to produce an 18/19-digit code. Collection codes have a separate format and purpose. Result queries retain the actual displayed code; a successful result and its acknowledgement are distinct outcomes. Cancellation cannot turn an aborted response into a paid result.

DZS, union-code and NFC collection-tag sessions own consultation, explicit confirmation and result state. Parsing caller-acquired input starts no radio. Unknown responses, ambiguous submissions and transport errors remain visible. Repeating an uncertain confirmation queries the existing business identifier rather than silently submitting another debit.

NFC receive calls accept caller-acquired URL/NDEF bytes and emit application reply bytes through registered callbacks. Wallet context selects NFC reply generation; awake/sleep/lock and quick-pay contexts retain their separate behavior. The caller must provide the terminal's NDEF framing and complete physical write/readback. SDK processing success and successful transport submission are not merchant payment evidence.

Connected BLE handling validates device signatures through the crypto package, assembles bounded packets and emits MTU-sized reply fragments. The integration owns GATT, advertising, final-fragment confirmation, cancellation and teardown. Exact 27-byte payment-result service data is checked against the current context. The ciphertext getter's compatibility length is output-only; normal payer strings need at least 44 actual output bytes.

## Transit and limits

Transit activity/list/status, cache lifecycle, online code refresh and supported offline certificate profiles use the same injected capabilities. Native secp192k1/SHA-1 and double/triple-SM2 formats use the crypto package. Downloaded `SCRIPT` and `ONLINE_PRIORITY` generators are unsupported. Other native certificate formats and raw merchant create/query protobuf payment remain unsupported or unqualified. Public declarations and finite regression coverage do not establish complete equivalence for all inputs or real issuer acceptance.

## Validation

Run the package's host tests without external Alipay inputs:

```sh
bazel test --config=macos_arm64 //libs/clients/alipay:all
```

`public_api_test` and `public_api_cpp_test` exercise the owned declarations, linkage, lifecycle and supported business calls. `crypto_compat_test` retains fixed mask/hash and derived-key ciphertext checks, in-place masking, output guards and allocator/provider failure cleanup. `payment_profile_test` verifies that omitted image/script capabilities preserve outputs, issue no script request and release their temporary resources. Local tests cover bounded buffers, malformed responses, allocation failures, cancellation, persistence and cleanup.

Seven immutable fixtures retain 10,416 synthetic input/output scenarios captured during previously passing machine-code cross-validation. [The fixture manifest](tests/fixtures/compatibility.json) preserves their original checksums, comparison counts and reference provenance. Replay compares 10,378 supported records. The transit test explicitly excludes 38 archived downloaded-script records; the scan test omits 23 additional image-decode comparisons derived from its text records. Both exclusions are reported. `transit_test` and `payment_profile_test` separately verify unsupported results, output preservation and absence of script requests. Compressed JSONL files are deliberate test data, not build artifacts or captured account material. Supported records compare complete returned buffers, requests, callback sequences and state through the real native implementation.

Replay requires neither the original binary nor QEMU. It does not rerun the original implementation, retain the previous inner-function differential checks, prove all public inputs equivalent or establish a real merchant transaction. Connected BLE issuer scenarios substitute a synthetic issuer only within the fixture and do not modify production trust roots. The declared image entry has composition coverage because the reference delivery had no matching image-entry implementation.
