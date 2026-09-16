# Main acceptance attempt — 2026-09-16

## Outcome and source

Incomplete: stopped at step 1 because the agent supplied an incorrect Bazel button target, before native compilation or any installation. This is an invocation failure, not evidence of a source compilation defect or a hardware failure. No retry or workaround was attempted under the requested first-failure stop rule.

Source HEAD and live origin/main both resolve to `667cd92585a92bcc81001090bbf90a5ed907ab10`; local branch is `jieli-main-acceptance`. The pre-existing untracked `lock` file was preserved. Older acceptance results are not reused as acceptance of this source.

## Step results

| Step | Result |
| --- | --- |
| 1: native packages and host tools | Native invocation exited 1 during target loading, 8.969 seconds, 0 processes; host CLI and lifecycle runner built successfully, exit 0, 5.579 seconds. |
| 2: install-loader trial/confirm/copy-back | Not run; no new Loader installed. |
| 3: untraced PAL | Not run; 0/10 cases executed, no aggregate result or case lines captured. |
| 4: UART lifecycle | Not run; 0/25 cases executed, result code unavailable. |
| 5: BLE lifecycle twice | Not run; 0/22 cases executed in either planned round, result codes unavailable. |
| 6: button App | Not run; READY and confirmation not captured. |
| 7: audio-system | Not run; READY, 30-second streaming and retained Runtime not verified. |
| 8: new Loader final state | Not reached; a separate read-only stop-state poll succeeded, but identifies the pre-existing Loader. |

## Build failure and identities

The native command ran in OrbStack `embed-zig-noble-amd64`, sourced `/Users/idy/h2vivi/firmwares-devenv/export.sh`, unset `IDF_PATH H2LOADER_IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH`, and used `bazel --output_user_root=/home/idy/.cache/bazel-ac791n/root build --config=ac791n --symlink_prefix=bazel-amd64-`.

The requested labels were:

```text
//projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package
//projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/button/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package
//projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package
```

Bazel reported `target 'package' not declared in package 'projects/example/targets/h2loader_tar_zlib/button/jieli_ac791n_devkit'`. Subsequent read-only inspection confirms this BUILD file declares `button_package`. The other requested labels were not all validated by this failed invocation. No corrected build was attempted.

| Artifact | Fresh package SHA-256 / image identity |
| --- | --- |
| Loader | Unavailable: native build did not complete. |
| PAL | Unavailable: native build did not complete. |
| button | Unavailable: invalid target label. |
| audio-system | Unavailable: native build did not complete. |
| display lifecycle fixture | Unavailable: native build did not complete. |
| crash-before-confirm lifecycle fixture | Unavailable: native build did not complete. |

No existing native output was attributed to the current source. Host tools built with `bazel build --config=macos_arm64 //projects/h2loader/targets/cc_binary/cli:h2loader //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner`; binary SHA-256s are `b360042996b46fff044e62cd70c73dc7491e54a285cec6b4ec4db33a35db5a8d` (CLI) and `3054e8a0e0b1ad02cd2ec9a3487e3000bb8571d95f797be24ecd34fcfa25b223` (runner). The host build had been launched before the native failure was inspected.

## Independent stop-state snapshot

`lsof /dev/cu.usbserial-20131240` found no readers before the independent `h2loader --no-ble --port /dev/cu.usbserial-20131240 --baud 460800 status` poll. The poll exited 0; no reset or installation preceded it. UID is `d879349abc9f`, active role Loader, running/next partition 1, Stage empty and `last_result=0`. This establishes responsiveness only, not acceptance of a freshly built main Loader.

```text
H2_LOADER_STATUS board=jieli_ac791n_devkit target=wl82 chip=ac791n device_uid=d879349abc9f capabilities=0x00000005 command_availability=0x00081d1f active_role=loader active_version=bazel-native-artifacts active_checksum=eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017 active_image_size=938473 running_partition=1 next_partition=1 boot_intent=loader stage_valid=0 stage_package_checksum=- stage_package_size=0 stage_image_checksum=- stage_image_size=0 stage_role=unknown stage_version=- stage_board=- stage_target=- partition_1_valid=1 partition_1_package_checksum=f96cf7e5ec91b66aa7aba0c2ff1c46c710f7784c24419ffca3ebd8fcdf9fea7a partition_1_package_size=927639 partition_1_image_checksum=eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017 partition_1_image_size=938473 partition_1_role=loader partition_1_version=bazel-native-artifacts partition_1_board=jieli_ac791n_devkit partition_1_target=wl82 partition_2_valid=1 partition_2_package_checksum=b1fde0e35f8f2d74c65b3fba3aa9270b937a2ca14ca938ff488952e5a5421839 partition_2_package_size=890582 partition_2_image_checksum=111475df6c2d2aa7509683f08bf204a7ffd945d1e2d5fa75ea747780c79cf033 partition_2_image_size=901833 partition_2_role=app partition_2_version=bazel-native-artifacts partition_2_board=jieli_ac791n_devkit partition_2_target=wl82 last_result=0 mfg_mode=1 mfg_steps=0000000000000000000000
```

## Limits and retained evidence

Wi-Fi credential persistence was explicitly skipped because no bench AP credentials were provided. All hardware suites remain unexecuted due to the step-1 stop. There are no PAL, button, audio or Loader trial capture lines; no software reset, power cut, manual power cycle, USB DL, format operation, other-port access, BLE install or Terminal BLE invocation occurred. The planned BLE endpoint was `5:818f070641f0`; it was not accessed or revalidated. No 90-second post-reset health window arose because no reset occurred.

Raw build logs and the independent status capture remain outside the worktree under `/tmp/jieli-main-acceptance-2026-09-16`; all review-relevant facts are inline above. No raw log, status or JSON capture was added to the worktree.

## Documentation validation

`bazel test --config=macos_arm64 //guides:guides_build_test //tools/bazel:jieli_evidence_test` exited 0: both tests passed (guides test cached, evidence test freshly executed), overall 12.929 seconds. `git diff --check` passed. These checks validate documentation only.
