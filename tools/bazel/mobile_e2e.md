# Packaged mobile E2E runners

`mobile_e2e_test` declares a direct `py_test`; every target uses the same `tools/bazel/mobile_e2e.py` main. The App and `app_sdk` stay in `ios_sim_arm64` / `android_arm64` configuration; `mobile_e2e_host_python` selects the repository's hermetic Python runtime in execution configuration. Python libraries use `HOST_OR_MOBILE_TOOL_COMPATIBILITY`: they accept matching host configurations and the supported iOS/Android configurations, while embedded/K4B graphs skip them because those targets have no Python runtime. No shell trampoline or undeclared system Python is involved.

Invoke the suite's exact Bazel label directly. `mobile_e2e_test` accepts `external = True`; live qualification targets set this option so execution is always fresh while artifact build caches remain enabled:

```sh
bazel test --config=ios_sim_arm64 --cache_test_results=no \
  //projects/e2e/targets/ios_application/pal-core:ios_pal_core_simulator_test
bazel test --config=android_arm64 --cache_test_results=no \
  //projects/e2e/targets/android_binary/pal-core:android_pal_core_simulator_test
bazel test --config=macos_arm64 //tools/bazel:mobile_e2e_runtime_test
```

Keep the configured disk cache enabled. Set `H2_IOS_SIMULATOR_UDID` or `H2_ANDROID_SERIAL` to an explicitly reserved, booted simulator. Android also uses `ANDROID_HOME`; symbol probes require `ANDROID_NDK_HOME`. These tests are manual and local: simulator side effects are not remote actions. A consumer can declare `external = True` on `mobile_e2e_test` to disable cached test results through Bazel's native `external` tag while preserving build caching; the compatibility default leaves existing consumers unchanged. Use `--local_test_jobs=1` when running several live suites on one simulator, and reserve devices across independent Bazel invocations. The runner does not boot, reset or erase devices.

## Adding a suite

Declare one `mobile_e2e_suite` in the suite owner's `BUILD.bazel`. Package/report names, timeout, registry pattern/count, expected report fields, case result field (`rc` or `detail`), resource balance, permissions and SDK/consumer symbol requirements are data. The rule emits a JSON declaration and carries its registry, optional hook, fixture files and Python dependencies in runfiles. Both platform consumers reuse that declaration; a Python suite file is optional.

```starlark
load("//tools/bazel:mobile_e2e.bzl", "mobile_e2e_suite", "mobile_e2e_test")

mobile_e2e_suite(
    name = "mobile_e2e",
    package = "com.example.e2e",
    report = "example-result.json",
    registry = ":cases.inc",
    registry_pattern = 'EXAMPLE_CASE\\("([^"]+)"',
    expected = {"failed": 0, "rc": 0},
)

mobile_e2e_test(
    name = "ios_example_simulator_test",
    platform = "ios",
    suite = ":mobile_e2e",
    app = ":example.ipa",
    sdk = ":app_sdk",
)
```

The common Python entrypoint reads the declaration, installs the consumer, checks SDK identity, grants declared permissions and runs the App. Its fixed report contract requires a nonempty unique registry, its declared count when fixed, an exactly ordered case ledger, every case `PASS` with zero `rc`/`detail`, exact expected fields, platform identity and actual `passed` count. A suite may declare `optional_cases` as a unique subset of its registry: only those cases may report `UNSUPPORTED` with typed `H2_PAL_ERR_UNSUPPORTED` (`-3`), recorded as skips instead of passes. The default empty list keeps every case mandatory. Timeout, IO, failed cleanup and `NOT_ASSESSED` cannot satisfy a skip; supported optional cases still require PASS. `resource_balance` additionally checks the `before` and `after` resource snapshots. These checks always run, including after custom hooks; no hook can replace or bypass them. Starlark only declares/builds inputs; it does not execute assertions or contain an assertion language.

Use `hook` only for imperative differences. `run_suite(app, args)` may own a multi-process plan or controlled service and return a report; `parse_report(raw)` handles special report syntax; `verify_report(report, args)` adds business assertions after standard validation. `args.contract` contains the declaration and hook-specific `options`; `args.registry` and `args.fixtures` resolve declared inputs. Fixture Python imports belong in `deps`, and fixture executables in `fixtures = {"server": "//owner:server"}`. Extra environment variables are explicit on the consumer test.

Core, Crypto, JSON and Audio Decoder use no Python hook. Core declares resource balance; JSON and Audio Decoder declare platform SDK/consumer probes. Storage retains its two-process persistence protocol; HTTP and WebRTC retain live service/peer evidence; Audio retains frame, stability and allocator assertions; Display retains only its native line-report parser. All their static parameters live in their owning BUILD declarations, not in a central suite table.

`launch` removes the old result, starts a new process, waits under one deadline, collects raw reports/logs, then terminates in `finally`. It is deliberately repeatable: Storage installs once, clears its own sandbox once, and launches phases 1 and 2 with one nonce. Storage itself checks distinct PIDs, every phase/nonce/case, persisted values and final sandbox cleanup. Reports from successive launches use numbered filenames instead of overwriting phase 1. Incomplete parsers raise `ValueError`; suite assertions must not be used to signal an incomplete report.

`MobileApp` also closes after partial installation or initialization failure. Log/fixture cleanup errors do not skip App termination or replace the original failure. `failure.json` records failures and secondary cleanup errors; `environment.json` records package hashes, device/runtime identity, timestamps, launch count and runner status. A failure removes `qualified.json`; raw reports remain diagnostic data. The environment also records the exact generated suite declaration SHA-256. Each subprocess is bounded, and command time during launch/polling is capped by the phase deadline. Fixture context managers remain responsible for terminating their controlled services.

Android checks the actual arm64 `.so` bytes in APK against AAR before install. `verify_android_sdk` additionally checks required headers and exported symbols. `verify_ios_symbols` checks the suite's provider symbols in both the static XCFramework and linked IPA plus the portable App symbol, recording binary hashes. A static iOS link is not a byte-for-byte archive comparison.

Other suites can adopt the same interface with their own fixtures and qualification assertions. Historical hardware evidence is not revalidated by a runner refactor. New consumer results must record the new revision, platform and package identities separately from old qualification records.
