# Testing programs and artifacts

This project owns portable test programs, device orchestration, fixtures and final testing artifacts. Manufacturing acceptance belongs to the H2Loader MFG App. Ordinary source unit tests remain beside the source they exercise.

Programs are grouped by what they validate:

| Category | Ownership |
| --- | --- |
| `unit/<suite>/app` | Single-board correctness programs, including PAL suites using a local PC controller, AP or HTTP fixture and AEC calibration using an independent acoustic source. Each suite owns its private `libs/` helpers. |
| `e2e/<suite>` | Cooperating DUT programs: iPerf client/server, Lua link host/join and board-to-board IPv6. |
| `integration/<suite>/app` | Programs with real external services or Internet dependencies: GizClaw, OneNET, Lingxi and Net/TLS (including its mandatory public DNS case). |
| `benchmark/<suite>/app` | Performance programs, including WebRTC throughput. Single-board iPerf matrix firmware also belongs to the benchmark artifact category. |
| `libs/<name>` | Named support used across suites: host Runtime, mobile platform configuration, Wi-Fi fixture inputs and H2Loader orchestration. |

`e2e/iperf/{client_app,server_app,libs}` owns both DUT roles; its generic matrix implementation is `libs/runner`. `e2e/lua_link/{host_app,join_app,libs}` composes both roles over one session implementation. Lua role selection remains in the existing launcher and `H2_LUA_LINK_E2E_ROLE` configuration.

All final artifact entries use an artifact-kind-first tree:

```text
targets/h2_loader_tar_zlib/<category>/<suite>/<role-if-needed>/<manufacturer>/<board>/
targets/h2_loader_zlib_tar/<category>/<suite>/<role-if-needed>/<manufacturer>/<board>/
targets/cc_binary/<program>/
targets/cc_test/<program>/
targets/android_binary/<program>/
targets/ios_application/<program>/
targets/pkg_tar/<program>/
```

Format-1 packages live under `h2_loader_tar_zlib`; format-2 packages live under the matching `h2_loader_zlib_tar` directory. Native ESP-IDF/BK7258/JieLi rules, startup, Runtime assembly, profiles, SDK inputs, versions and task policy remain owned by the tar_zlib entry. Standalone diagnostics keep their existing native firmware rule in that tree. H2Loader manufacturing peer/calibration sources remain inside their owning native H2Loader board entry directory.

The tar_zlib entry builds the native image and format-1 package. The zlib_tar entry borrows its raw native/data inputs through `source` and builds format 2 without building the format-1 archive. Both directories expose `:package`; DevKit's Loader E2E App uses `:package_usb` and `:package_uart` for its two consoles. Board, image, role, version and protocol identities stay the same. Captured evidence, qualification JSON, historical binding tables, fixed-source recipes and binary assets retain their original bytes and historical paths; they are records of their bound source, not current entry aliases.

```sh
bazel build --config=esp32s3 //projects/testing/targets/h2_loader_zlib_tar/unit/pal-core/espressif/esp32s3_devkitc_1:package
bazel run //projects/testing/tools:list_apps -- --repository "$PWD"
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind unit
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind e2e
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind integration
```

The inventory accepts `unit`, `e2e`, `integration` and `benchmark`; role paths are reported separately from physical manufacturer/model names. Entry queries default to `h2_loader_tar_zlib`; pass `--artifact-kind h2_loader_zlib_tar` with `--kind` to list format-2 entries. Manufacturing belongs to the [H2Loader MFG App](../h2loader/apps/mfg/README.md). See [suite coverage](suites.md) and each suite's README/public headers for acceptance and lifetime contracts. Repository constraints are indexed in [guides](../../guides/index.md).
