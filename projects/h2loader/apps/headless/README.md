# Headless Loader App

This App owns portable Loader startup and command composition. Public contracts live in [`include/`](include), implementation in [`src/`](src), and lifetime regressions in [`tests/`](tests). [`BUILD.bazel`](BUILD.bazel) retains the logical `:loader` library, command task declarations and version `0.2.3`.

The App uses Runtime, PAL and portable H2Loader/command libraries. [`../../libs/device_service`](../../libs/device_service) composes it with the Device core and exposes the explicit native source inventory. Concrete board startup, provider selection, task policies and packages belong to [`../../targets/h2_loader_tar_zlib/loader`](../../targets/h2_loader_tar_zlib/loader).

```sh
bazel test --config=macos_arm64 //projects/h2loader/apps/headless:lifetime_test
bazel build --config=esp32s3 \
  //projects/h2loader/targets/h2_loader_tar_zlib/loader/espressif/esp32s3_devkitc_1:package
```

Host tests use declared host dependencies; firmware packages require the matching pinned SDK/toolchain. Package builds do not flash devices or establish hardware qualification. Public C APIs and existing Loader image/package identities remain defined by their contracts and final entries.
