# SemVer comparison

Portable C11 validation and precedence comparison following [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html), with an application policy that treats every invalid version (including empty strings and NULL) as equally lowest. No PAL dependency, heap allocation, mutable global state, or integer conversion is required. Numeric components can exceed machine integer widths. Runtime is linear in input length and working memory is constant.

Depend on `//libs/semver` and include `h2_semver.h` (C and C++ supported):

```c
#include "h2_semver.h"

int order;
if (h2_semver_compare("1.2.3-rc.1", "1.2.3", &order)) {
    /* order == -1: the release candidate precedes the release. */
}
```

Comparison accepts invalid versions as the lowest rank; callers that need to reject them use strict validation first. Build metadata is validated but ignored for ordering. Non-NULL inputs must be NUL-terminated and remain valid and immutable during the call. Version ranges and automatic tag-prefix removal are outside this library.

See the [development guide](../../guides/zh/developing/semver.md) for ownership and behavior. Public API parameters and return values are documented in [h2_semver.h](include/h2_semver.h), the source for generated API reference pages.

Run host tests with `bazel test --config=macos_arm64 //libs/semver:all` (or `--config=linux_x86_64` / `--config=windows_x86_64` on those hosts).
