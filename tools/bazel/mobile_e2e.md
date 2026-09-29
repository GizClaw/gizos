# Packaged mobile E2E runners

`mobile_e2e_test` declares a direct `py_test`. The App and `app_sdk` stay in `ios_sim_arm64` / `android_arm64` configuration; `mobile_e2e_host_python` selects the repository's hermetic Python runtime in execution configuration. Python libraries contain host source, so they must not require the App's target OS to be a desktop OS. No shell trampoline or undeclared system Python is involved.

Use the existing Make targets, or invoke the same Bazel labels explicitly:

```sh
bazel test --config=ios_sim_arm64 --cache_test_results=no \
  //projects/e2e/targets/ios_application/pal-core:ios_pal_core_simulator_test
bazel test --config=android_arm64 --cache_test_results=no \
  //projects/e2e/targets/android_binary/pal-core:android_pal_core_simulator_test
bazel test --config=macos_arm64 //tools/bazel:mobile_e2e_runtime_test
```

Keep the configured disk cache enabled. Set `H2_IOS_SIMULATOR_UDID` or `H2_ANDROID_SERIAL` to an explicitly reserved, booted simulator. Android also uses `ANDROID_HOME`; symbol probes require `ANDROID_NDK_HOME`. These tests are manual and local: simulator side effects are not remote actions. Use `--local_test_jobs=1` when running several live suites on one simulator, and reserve devices across independent Bazel invocations. The runner does not boot, reset or erase devices.

## Adding a suite

Declare the suite Python file as `runner`, imported Python libraries in `deps`, and all runtime inputs in `data`: IPA/APK, SDK, registry, fixture server and any fixture files. Pass artifact locations with `$(rootpath ...)` in `args` and inherit only the environment needed by the suite. Keep the native consumer's existing platform compatibility. `mobile_e2e_test` adds the shared runtime dep.

```starlark
load("//tools/bazel:mobile_e2e.bzl", "mobile_e2e_test")

mobile_e2e_test(
    name = "ios_example_simulator_test",
    runner = "//projects/e2e/libs/example-mobile:run_mobile.py",
    args = ["ios", "$(rootpath :example.ipa)", "$(rootpath :app_sdk)",
            "$(rootpath :registry)"],
    data = [":example.ipa", ":app_sdk", ":registry"],
    env_inherit = ["H2_IOS_SIMULATOR_UDID", "DEVELOPER_DIR"],
    target_compatible_with = IOS_SIM_ARM64_ARTIFACT_COMPATIBILITY,
)
```

The suite owns its CLI, fixture, phase plan, permissions, report parser and PASS oracle. Keep verification **inside** the transaction and publish qualification only after every suite assertion, including fixture-side evidence, succeeds:

```python
from tools.bazel.mobile_e2e import MobileApp, save_evidence

with MobileApp(args.platform, args.app, args.sdk, PACKAGE,
               "example-result.json", args.output, timeout=args.timeout) as app:
    with app.fixture("fixture.json", settings_json):
        report = app.launch()  # parse=custom_parser for non-JSON reports
    verify(report, args.registry)
    save_evidence(args.output, report, app.environment(), args.app, args.sdk)
```

`launch` removes the old result, starts a new process, waits under one deadline, collects raw reports/logs, then terminates in `finally`. It is deliberately repeatable: Storage installs once, clears its own sandbox once, and launches phases 1 and 2 with one nonce. Storage itself checks distinct PIDs, every phase/nonce/case, persisted values and final sandbox cleanup. Reports from successive launches use numbered filenames instead of overwriting phase 1. Incomplete parsers raise `ValueError`; suite assertions must not be used to signal an incomplete report.

`MobileApp` also closes after partial installation or initialization failure. Log/fixture cleanup errors do not skip App termination or replace the original failure. `failure.json` records failures and secondary cleanup errors; `environment.json` records package hashes, device/runtime identity, timestamps, launch count and runner status. A failure removes `qualified.json`; raw reports remain diagnostic data. Each subprocess is bounded, and command time during launch/polling is capped by the phase deadline. Fixture context managers remain responsible for terminating their controlled services.

Android checks the actual arm64 `.so` bytes in APK against AAR before install. `verify_android_sdk` additionally checks required headers and exported symbols. `verify_ios_symbols` checks the suite's provider symbols in both the static XCFramework and linked IPA plus the portable App symbol, recording binary hashes. A static iOS link is not a byte-for-byte archive comparison.

Core/Crypto preserve their registry oracles; HTTP retains real HTTP/HTTPS arrivals and rejection of a separate untrusted certificate; WebRTC retains its isolated Pion UDP peer and native fingerprint-authentication evidence; Audio explicitly grants microphone permission; JSON and Audio Decoder keep packaged provider probes; Display keeps its line-oriented parser, provider probes and PNG capture. These are suite code, not entries in a global configuration table.

Other suites can adopt the same interface with their own fixtures and qualification assertions. Historical hardware evidence is not revalidated by a runner refactor. New consumer results must record the new revision, platform and package identities separately from old qualification records.