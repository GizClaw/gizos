# Build and validation

Use Bazel as the source of truth for dependencies, toolchains, compatibility, builds, tests, and artifacts. Read [the build instructions](../tools/bazel/README.md) for configuration and environment setup; discover targets from the live graph rather than maintaining another inventory.

## Build constraints

- Each target has a clear source owner and public purpose. Split variants only for real differences in sources, definitions, configuration, toolchain, or dependencies.
- Use `_embed`, `_desktop`, `_mobile`, and `_web` for real runtime variants. Do not copy shared libraries per App or label native code as WebAssembly.
- Keep dependencies directed from consumers through their first-party owner to vendor code. External repository targets must not depend back on first-party GizOS labels.
- Vendor overlays expose upstream sources and headers; the first-party owner selects integration and platform dependencies. A pure upstream build may remain in its overlay when it requires no GizOS contract or platform wiring.
- Declare sources, headers, data, and dependencies explicitly. Preserve include ordering when a test header must override a production header.
- Use `firmware_native_component` for code compiled by a native SDK and one `firmware_lib_component` per firmware entry to collect Bazel libraries. Keep compile, link, SDK configuration, and lifecycle in the native entry.
- Keep native SDK versions and inputs declared. Native actions use explicit environment allowlists and declared outputs, run on the current runner with `no-sandbox` and `no-remote-exec`, and do not flash, reset, or monitor devices.
- Cache configuration changes performance only. Keep compiler caches in normal per-user cache directories, separate from SDK installations; exclude secrets and credentials from action keys, logs, and artifacts.

## Test and CI constraints

- Ordinary tests have no custom tags. Tests requiring real devices, external services, or manual preparation use `manual`; live tests may also use Bazel's `external` tag to disable result caching.
- Invoke manual tests by exact Bazel label. Do not add forwarding Make targets, shell dispatchers, custom tag filters, or test suites to maintain another test inventory.
- Each CI execution class requests the full compatible build and automatic test graph. Compatibility and caching may skip actions, but must not skip graph analysis. Required CI fails on failed, skipped, or cancelled classes.
- Validate public libraries through their own host tests and consumers. Build the final target when changing platform wiring; compiling an intermediate library alone does not validate its launcher.
- Cover meaningful behavior, malformed input, partial initialization, cancellation, cleanup, and concurrency boundaries. Keep ordinary unit tests independent of private services and hardware.
- Device claims require actual flash, reset, serial, interoperability, or coredump evidence appropriate to the change. Report unavailable checks and their limits explicitly.

## Before handing off

Run focused checks for the change and the repository formatter check:

```sh
make format-check
git diff --check
```

Run `make format` before committing. Update affected build inputs and relative links when adding, moving, or deleting files. Keep generated outputs, caches, logs, credentials, and machine-local configuration out of the diff. There is no Guides website or documentation build to run.
