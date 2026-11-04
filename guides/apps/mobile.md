# Mobile

Mobile entries belong to the portable App's project owner. Browser/WebAssembly follows the same ownership principle and has a separate [Web contract](/apps/web).

## Structure

The following paths describe the ownership pattern; `<owner>` and `<app>` are placeholders for a consumer's actual project and App:

```text
projects/<owner>/
├── apps/<app>/app/                  # Portable App contract and implementation
├── libs/<mobile-adapter>/           # App-specific presentation or contract conversion
└── targets/
    ├── ios_application/<app>/       # iOS entry and package
    └── android_binary/<app>/        # Android entry and package

libs/pal/providers/
├── ios/                            # Reusable iOS PAL implementations
└── android/                        # Reusable Android PAL implementations
```

An App-specific adapter owns presentation and contract conversion only when the App needs them. The artifact entry owns platform lifecycle, Runtime assembly, package identity and the final output. Reusable surface, display, input and other platform implementations stay in `libs/pal/providers/<platform>/`.

## Platform boundaries

- iOS providers own UIKit-backed capabilities; the `ios_application` entry owns UIApplication lifecycle, Simulator bundle metadata and the App thread.
- Android providers own reusable JNI-backed capabilities; the `android_binary` entry owns Activity callbacks, APK packaging and the App thread.

Each App owns its supported artifact entries because package identity, required capabilities and native startup belong to that App. Entries consume the shared PAL providers and do not copy their implementations. Direct LVGL consumers use the reusable `//libs/lvgl:lvgl_mobile` variant and `//libs/lvgl:single_thread_config`; third-party overlays provide upstream sources rather than App-specific library variants.

## Bazel build boundary

Mobile artifacts use the real `rules_apple`, `rules_android` and `rules_android_ndk` dependency graphs. Platform compatibility skips unsupported artifacts in host builds. iOS artifacts require Xcode and the `ios_sim_arm64` configuration; Android artifacts require the SDK/NDK and `android_arm64` configuration. Building a package proves its declared build and packaging contract, not execution on a device.

Retained PAL E2E entries consume the packaged SDKs and run the portable suite in a prepared Simulator or Emulator. For example, PAL Core uses:

```sh
bazel test --config=ios_sim_arm64 \
  --test_env=H2_IOS_SIMULATOR_UDID=<booted-uuid> \
  //projects/e2e/targets/ios_application/pal-core:ios_pal_core_simulator_test
bazel test --config=android_arm64 \
  --test_env=H2_ANDROID_SERIAL=<emulator-serial> \
  //projects/e2e/targets/android_binary/pal-core:android_pal_core_simulator_test
```

These manual tests require a dedicated, already running test device. Simulator results remain separate from physical-phone acceptance.

## Packaged PAL E2E

Core, Storage, Crypto, HTTP, WebRTC, JSON, Audio, Audio Decoder and Display share `tools/bazel/mobile_e2e.py` and the `mobile_e2e_test` macro. Their Python tests use the same public entrypoint; a suite adds a hook only for its specific fixture, parsing or assertions. `mobile_e2e_suite` declares package identity, reports, registry format, expected fields, deadlines, permissions and SDK symbols in the suite's BUILD package. Both platform entries reuse that declaration, and runfiles and Python dependencies follow the Bazel graph.

The shared runner owns installation, old-report removal, launch, timeout, logs, failure records, termination and SDK artifact identity. Each suite retains its complete registry and PASS oracle: Storage owns persistence phases and nonce checks; HTTP/WebRTC own controlled services, TLS/fingerprint rejection and peer evidence; Audio owns permission setup; Display, JSON and Audio Decoder own their report and provider-symbol checks. The full integration contract is in `tools/bazel/mobile_e2e.md`.

A failed raw report or `failure.json` is diagnostic. Qualification requires all suite assertions and cleanup to succeed, and new execution evidence retains its own source and artifact identity.

Raw Net/TLS consumers may create a dedicated native `h2_ios_net` or `h2_android_net` owner exported by the packaged SDK. It holds a shared WolfSSL lifecycle reference; the API is borrowed until owner destroy, after the consumer closes all sockets/resolvers and quiesces callers. The Net/TLS suite uses the same declarative mobile runner with only live fixture/peer assertions in its hook. Optional ICMP/multicast cases are named in Bazel; only typed `UNSUPPORTED` can be skipped, while the 37 supported mandatory cases and cleanup must pass.

## Production acceptance

Each production App must verify its required provider capabilities and define permissions, foreground/background lifecycle, release signing and physical-device acceptance. The [E2E guide](/apps/e2e) records the retained suite boundaries; a passing subset or a built artifact does not establish complete mobile-platform qualification.
