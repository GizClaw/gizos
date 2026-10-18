# BK SHA-384 certificate regression

This test builds the actual pinned SDK mbedTLS sources with its minimal configuration and the PUBLIC feature definitions extracted from the production BK component. Generated host include shims substitute OS headers and host allocation/logging/threading hooks and OS entropy; they do not enable SHA-384 independently or change certificate verification. The test uses a known SHA-384 digest and a local SHA-384-signed certificate chain, then rejects a changed signature and an unrelated trust anchor. Fixtures contain public certificates only.

```sh
cmake -S native_component_src/bk7258/ap/h2_pal_core/tests/tls_host -B /tmp/bk-sha384 \
  -DBK7258_PATH="$BK7258_PATH" -DH2_REPO_ROOT="$PWD"
cmake --build /tmp/bk-sha384 -j 4
ctest --test-dir /tmp/bk-sha384 --output-on-failure
```

Repeat with a separate build directory and `-DDISABLE_SHA384=ON`. Both configurations execute the same real digest and certificate parse/verification operations. In the disabled build, CTest passes `--expect-unavailable` and requires both operations to reject; running that binary without the argument must return nonzero. There is no preprocessor early exit or independently enabled digest feature in the probe. SDK source stays unchanged. This local SDK-dependent regression does not establish physical HTTPS or network interoperability; the standard native BK package still builds with its own board configuration.
