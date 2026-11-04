# Cross-platform E2E Apps

This project owns reusable test Apps, device-test orchestration, fixtures and
final platform entries. Portable registries live in `apps/<app>/app`; reusable
support lives in named `libs/<library>` packages.

Firmware entries use `targets/<app>/<manufacturer>/<board>`. Manufacturer and
model names match the physical BSP under `boards/<manufacturer>/<board>`.
For example:

```text
targets/pal-core/espressif/esp32s3_devkitc_1/
targets/pal-core/beken/bk7258_v3_202405/
targets/pal-audio/waveshare/esp32s3_touch_amoled_1_8/
```

Each entry owns native startup, Runtime assembly, task policy and its BUILD/CMake
inputs. `:firmware` builds the native image; `:package` retains the format-1
H2Loader package. Entries with format-2 support expose `:package_v2` in the same
BUILD file. Both packages reuse one native firmware graph. Directory names do
not change the board/image identities stored in packages or reported on the wire.
Standalone diagnostics expose their native firmware target in the same layout.

```bash
bazel build --config=esp32s3 //projects/e2e/targets/pal-core/espressif/esp32s3_devkitc_1:package_v2
```

Host, mobile, browser and archive artifacts retain their existing artifact-rule
roots, such as `targets/cc_binary/<app>` and `targets/pkg_tar/<app>`.
The native H2Loader test runner is
`//projects/e2e/targets/cc_binary/e2e-runner:e2e-runner`. Shared Wi-Fi fixture inputs
live in `libs/fixture-wifi`; carrier provisioning helpers live in `targets/carrier`.

List portable Apps with:

```bash
bazel run //projects/e2e/tools:list_apps -- --repository "$PWD"
```

Run deterministic inventory checks without an SDK or device:

```bash
bazel test --config=macos_arm64 //projects/e2e/tools:inventory_test
```

Vendor firmware builds require their configured SDK/toolchain and supported host.
A successful build or host test does not establish physical-device acceptance.
See each App's README and public headers for lifecycle, fixture and qualification contracts. Repository constraints are indexed in [guides](../../guides/index.md).
