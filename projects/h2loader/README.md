# H2Loader firmware and artifacts

This project owns the portable Loader App, thin firmware startup/composition,
Loader image entries, browser npm artifact and firmware package rules. Shared
libraries are organized as
[`libs/h2loader/{host,device,web}`](../../libs/h2loader), with one common Device
service over PAL.

| Directory | Responsibility |
| --- | --- |
| `apps/loader/app` | Portable Loader App entry and command/startup composition |
| `targets` | Firmware and npm artifact composition, minimal SDK startup registration |
| `tools/bazel` | Firmware package, recovery and release artifact rules |

Vendor image/update, boot, Flash/cache and physical transport mechanisms belong
to platform PAL providers under [`native_component_src`](../../native_component_src).
Firmware entries are grouped as `targets/<app>/<manufacturer>/<board>`, for
example `targets/loader/espressif/esp32s3_devkitc_1`. Each BUILD file owns the
native firmware and its package variants: `:package` retains format 1 and
`:package_v2` provides format 2 where supported. Both reuse the same image graph.
Directory manufacturer/model names match the physical BSP; package board and
image identities retain their existing values.

Physical wiring and layout geometry belong to
[`boards/<company>/<board>`](../../boards). The project has no native business
code tree or chip-specific Loader components. Final entries call the common
Device service with public Runtime capabilities. Generic `firmware_lib_component`
source declarations handle vendor C-runtime ABI requirements.

BK7258 native image encryption belongs to `//tools/bazel:bk7258.bzl` and its
`bk7258_encrypted_firmware` rule; it also accepts non-H2Loader firmware. This
project's `:repackage` command accepts the already processed image and its native
receipt, preserves managed-package data/identity or adds a `.h2fb` envelope, and
does not read keys or perform encryption.

The native command-line implementation and OS adapters belong to
[`tools/gizloader`](../../tools/gizloader). Cross-device test Apps, orchestration
and firmware fixtures belong to [`projects/e2e`](../e2e). The product Batch Loader
UI belongs to `GizClaw/www`; this project's npm target packages the shared Web SDK
as `@gizclaw/h2loader`.

## Build

Test shared code without a vendor SDK:

```bash
bazel test --config=macos_arm64 //libs/h2loader/device:h2loader_pal_test
```

Build a final firmware package through its target, for example:

```bash
bazel build --config=esp32s3 //projects/h2loader/targets/loader/espressif/esp32s3_devkitc_1:package
```

Native builds require a configured vendor SDK/toolchain and supported native
host. Package builds do not flash devices or publish releases. Host fixtures and
build-graph analysis do not establish hardware acceptance.

See the [shared library README](../../libs/h2loader/README.md), public headers
and the owning BUILD targets for contracts and lifecycle ordering. Repository
constraints are indexed in [guides](../../guides/index.md).
