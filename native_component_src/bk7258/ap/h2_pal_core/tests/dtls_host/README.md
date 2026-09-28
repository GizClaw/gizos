# BK DTLS host regression

This diagnostic builds the real BK PAL DTLS source with the mbedTLS source already
present in `BK7258_PATH`. It substitutes only host allocation, OS entropy, logging
and an in-memory datagram transport. It does not copy SDK algorithms or change
the SDK checkout. It is a provider regression, not physical-board qualification
or a substitute for the independent Pion WebRTC gate.

The SDK omits upstream's top-level generators, so the test directly includes its
library build. It uses SDK upstream default software crypto, enables the same
DTLS-SRTP feature as the firmware, disables peer-certificate retention as in the BK
minimal profile, and disables upstream self-tests that reference
SDK-only RNG symbols. Firmware still builds with its own board configuration.

On macOS or Linux, with CMake and ccache available, from the repository root:

```sh
cmake -S native_component_src/bk7258/ap/h2_pal_core/tests/dtls_host \
  -B /tmp/bk-dtls-contract \
  -DBK7258_PATH="$BK7258_PATH" -DH2_REPO_ROOT="$PWD" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build /tmp/bk-dtls-contract -j 8
ctest --test-dir /tmp/bk-dtls-contract --output-on-failure
```

The test requires an actual HelloVerifyRequest, repeats the handshake after a
dropped cookie response, checks matching SRTP exporter output and binary records
in both directions, and checks retry after output backpressure. Both server-side
and client-side incorrect fingerprints must reject the session and prevent SRTP
key export. A client that omits its certificate must also be rejected before any
application data or key export. Repeated session cleanup must release all provider-owned allocations.
Certificate absence or mismatch remains an authentication failure; no case uses
`VERIFY_NONE` or a fake DTLS provider.
