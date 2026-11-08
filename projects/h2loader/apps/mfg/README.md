# Manufacturing acceptance App

This App owns portable manufacturing acceptance, its model, session and UI. Public contracts live in [`include/`](include), implementation in [`src/`](src), and source regressions in [`tests/`](tests). [`BUILD.bazel`](BUILD.bazel) defines `:mfg`, `:model`, `:session` and version `0.1.49`.

Concrete profiles, SDK startup, task policies and firmware packages belong to [`../../targets/h2_loader_tar_zlib/mfg`](../../targets/h2_loader_tar_zlib/mfg). Each board's `profile.c` stays beside its final entry. Shared peer and acoustic-fixture support belongs to [`../../libs/mfg-peer`](../../libs/mfg-peer) and [`../../libs/mfg-aec-source`](../../libs/mfg-aec-source).

```sh
bazel test --config=macos_arm64 //projects/h2loader/apps/mfg:all
bazel build --config=esp32s3 \
  //projects/h2loader/targets/h2_loader_tar_zlib/mfg/espressif/esp32s3_devkitc_1:package
```

Host tests need the declared portable dependencies and host providers. Firmware packages additionally require the matching pinned SDK/toolchain. Building a package does not establish physical acceptance.

See the [operator guide](operator.md) for installation, checks, CLI commands, persistent results, peer fixtures, calibration and historical acceptance records. The App preserves H2Loader management and recovery while required checks gate product App startup.
