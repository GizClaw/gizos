# H2Loader host and build tooling

This directory owns H2Loader firmware packaging, recovery artifact generation and canonical release assembly. Portable protocol and Device/Host code belongs to [`libs/h2loader`](../../libs/h2loader); Apps and final firmware entries belong to [`projects/h2loader`](../../projects/h2loader).

- `h2loader_firmware.bzl` composes shared native ESP, BK and JieLi build rules.
- `h2loader_tar_zlib.bzl` and `h2loader_zlib_tar.bzl` expose both package formats from the same native image graph.
- `firmware_artifacts.py`, `firmware_output.py` and `h2loader_tar_zlib_runner.py` create packages, recovery files and release metadata.
- `release_bundle.bzl`, `release_bundle.py` and the cquery formatters assemble canonical firmware release inputs and outputs.
- `tests/` owns host regressions, synthetic build fixtures and frozen format-compatibility bytes.

```bash
bazel test --config=macos_arm64 //tools/h2loader:all
```

Host tests use local synthetic inputs and declared dependencies. Building actual firmware additionally needs the selected pinned SDK/toolchain. These tools do not flash devices or publish releases by themselves. Final entries retain ownership of App selection, Runtime lifetime, task policy and board/image/role/version identities.
