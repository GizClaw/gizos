# Testing programs and artifacts

This project owns manufacturing acceptance, portable test programs, device orchestration, fixtures and final testing artifacts. Ordinary source unit tests remain beside the source they exercise.

Programs are grouped by what they validate:

| Category | Ownership |
| --- | --- |
| `mfg/app` | Portable manufacturing acceptance App; `mfg/libs/peer` and `mfg/libs/aec-source` provide its peer and acoustic-source support. |
| `unit/<suite>/app` | Single-board correctness programs, including PAL suites using a local PC controller, AP or HTTP fixture. Each suite owns its private `libs/` helpers. |
| `e2e/<suite>` | Cooperating DUT programs: iPerf client/server, Lua link host/join, board-to-board IPv6 and AEC calibration using an independent source. |
| `integration/<suite>/app` | Programs with real external services or Internet dependencies: GizClaw, OneNET, Lingxi and Net/TLS (including its mandatory public DNS case). |
| `benchmark/<suite>/app` | Performance programs, including WebRTC throughput. Single-board iPerf matrix firmware also belongs to the benchmark artifact category. |
| `libs/<name>` | Named support used across suites: host Runtime, mobile platform configuration, Wi-Fi fixture inputs and H2Loader orchestration. |

`e2e/iperf/{client_app,server_app,libs}` owns both DUT roles; its generic matrix implementation is `libs/runner`. `e2e/lua_link/{host_app,join_app,libs}` composes both roles over one session implementation. Lua role selection remains in the existing launcher and `H2_LUA_LINK_E2E_ROLE` configuration.

All final artifact entries use an artifact-kind-first tree:

```text
targets/h2_loader_tar_zlib/<category>/<suite>/<role-if-needed>/<manufacturer>/<board>/
targets/h2_loader_tar_zlib/mfg/<manufacturer>/<board>/profile.c
targets/cc_binary/<program>/
targets/cc_test/<program>/
targets/android_binary/<program>/
targets/ios_application/<program>/
targets/pkg_tar/<program>/
```

The exact firmware directory name is `h2_loader_tar_zlib`. Native ESP-IDF/BK7258 firmware rules remain dependencies inside these packages. Standalone diagnostics keep their existing native firmware rule in the same artifact tree. Each board entry owns startup, Runtime assembly, profiles, SDK inputs, versions and task policy. Manufacturing peer/calibration variants remain inside the owning board directory.

`:firmware` builds the native image. `:package` and, where present, `:package_v2` stay together and reuse that image. The migration does not change board, image, role, version or protocol identities. Captured evidence, qualification JSON, historical binding tables, fixed-source recipes and binary assets retain their original bytes and historical paths; they are records of their bound source, not current entry aliases.

```sh
bazel build --config=esp32s3 //projects/testing/targets/h2_loader_tar_zlib/unit/pal-core/espressif/esp32s3_devkitc_1:package_v2
bazel run //projects/testing/tools:list_apps -- --repository "$PWD"
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind unit
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind e2e
bazel run //projects/testing/tools:list_apps -- --repository "$PWD" --kind integration
```

The inventory accepts `unit`, `e2e`, `integration`, `benchmark` and `mfg`; role paths are reported separately from physical manufacturer/model names. See [manufacturing](mfg/README.md), [suite coverage](suites.md) and each suite's README/public headers for acceptance and lifetime contracts. Repository constraints are indexed in [guides](../../guides/index.md).
