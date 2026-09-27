# Encoding

Portable C11 binary-to-text codecs modelled on Go's `encoding` packages: hex, base32, base64, and base85. One set of functions takes a prepared `h2_encoding_t` descriptor: `kind` picks the algorithm family, and the alphabet, padding, and optional base85 zero group pick the symbols. The descriptor carries its decode table, so calls do no per-call setup. Predefined descriptors cover RFC 4648 (hex, base32, base32hex, base64, base64url, with and without padding), Adobe Ascii85, and the RFC 1924 alphabet used by Python `base64.b85encode`. No PAL dependency, heap allocation, or mutable global state is required.

Depend on `//libs/encoding` and include `h2_encoding.h` (C and C++ supported):

```c
#include "h2_encoding.h"

char text[8];
size_t n;
if (h2_encoding_encode(&h2_encoding_base64_std, (const uint8_t *)"fo", 2,
                       text, sizeof(text), &n) == H2_ENCODING_OK) {
    /* text holds "Zm8=" (n == 4), not NUL-terminated. */
}

h2_encoding_t star; /* Go's StdEncoding.WithPadding('*'). */
h2_encoding_init(&star, H2_ENCODING_KIND_BASE32, h2_encoding_base32_std.alphabet, '*', '\0');
```

Encoding always produces canonical output and decoding accepts only canonical input, each in a single pass. `H2_ENCODING_ERR_NO_SPACE` reports the exact size needed and `H2_ENCODING_ERR_CORRUPT` reports the offset of the first bad byte; on either error the output buffer contents are unspecified. Line wrapping, Ascii85 `<~ ~>` framing, and streaming readers or writers are outside this library.

See the [development guide](../../guides/zh/developing/encoding.md) for ownership and behavior. Public API parameters and return values are documented in [h2_encoding.h](include/h2_encoding.h), the source for generated API reference pages.

Run host tests with `bazel test --config=macos_arm64 //libs/encoding:all` (or `--config=linux_x86_64` / `--config=windows_x86_64` on those hosts), and the CPU-time benchmark with `bazel run -c opt --config=macos_arm64 //libs/encoding:benchmark_encoding`.
